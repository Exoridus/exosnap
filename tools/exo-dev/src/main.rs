use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context as _, bail};
use clap::{Args, Parser, Subcommand};

use exo_dev::executor::{Context, DryRunExecutor, RealExecutor};
use exo_dev::git::Git;
use exo_dev::plan::{self, Event, PlanInput};
use exo_dev::process::StepRunner;
use exo_dev::profile::Profile;
use exo_dev::report::{self, ReceiptFacts};
use exo_dev::run::{self, Executor};
use exo_dev::scope::Scope;
use exo_dev::{hook, host_lock};

/// ExoSnap repository verification.
#[derive(Parser)]
#[command(name = "exo-dev", version)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Run a verification profile: the local contracts or a CI job's gate.
    Verify(Box<VerifyArgs>),
    /// Git hook entry points, called by the scripts in .githooks.
    Hook {
        #[arg(value_parser = ["pre-commit", "pre-push"])]
        name: String,
    },
    /// Standalone checks, also reachable individually outside a verify profile.
    Check {
        #[command(subcommand)]
        check: CheckCommand,
    },
}

#[derive(Subcommand)]
enum CheckCommand {
    /// The four build/Qt drift invariants: qt-version-consistency,
    /// setup-qt-centralized, no-qmake-project, qt-sdk-path-allowlist.
    Drift,
}

#[derive(Args, Default)]
struct VerifyArgs {
    /// The scoped pre-commit contract. It may check more than strictly needed and
    /// may never be falsely safe.
    #[arg(long, conflicts_with_all = ["full", "profile"])]
    fast: bool,
    /// The complete local blocking contract, at full scope.
    #[arg(long, conflicts_with = "profile")]
    full: bool,
    /// A named profile: pre-commit, pre-push, ci-lint, ci-guardrails, pr-policy,
    /// ci-dev-scripts, ci-build-debug, ci-build-release.
    #[arg(long)]
    profile: Option<String>,
    /// Scope the change set to what is staged, and let clang-format fix and
    /// re-stage.
    #[arg(long)]
    staged: bool,
    /// Commit to diff against. Locally defaults to the merge base with origin/next,
    /// then origin/main, origin/HEAD, next, main.
    #[arg(long)]
    base: Option<String>,
    /// CMake configure preset. Defaults to the profile's.
    #[arg(long)]
    preset: Option<String>,
    /// Build configuration. Defaults to the profile's.
    #[arg(long)]
    config: Option<String>,
    /// Cap the parallelism of cmake --build, ctest and clang-tidy. Defaults to
    /// EXOSNAP_VERIFY_JOBS, then all cores but two.
    #[arg(long)]
    jobs: Option<usize>,
    /// Where to write the receipt. Defaults to .workspace/verify/latest.json.
    #[arg(long)]
    result_path: Option<PathBuf>,
    /// Lines of a failed step's log printed at the end.
    #[arg(long, default_value_t = 120)]
    failure_tail_lines: usize,
    /// Execute nothing: every applicable check passes unless named by
    /// --simulate-fail.
    #[arg(long)]
    dry_run: bool,
    /// With --dry-run, the check names that report FAIL.
    #[arg(long, value_delimiter = ',', requires = "dry_run")]
    simulate_fail: Vec<String>,
    /// CI profiles: the GitHub event name.
    #[arg(long)]
    event: Option<String>,
    /// CI profiles: the pull request title.
    #[arg(long)]
    pr_title: Option<String>,
    /// CI profiles: the pull request number.
    #[arg(long)]
    pr_number: Option<String>,
    /// CI profiles: a packaging-relevant path changed in this pull request.
    #[arg(long)]
    packaging_changed: bool,
    /// CI profiles: the clang-tidy result cache directory.
    #[arg(long)]
    tidy_cache_dir: Option<PathBuf>,
}

fn main() -> ExitCode {
    match run_cli() {
        Ok(code) => code,
        Err(error) => {
            eprintln!("exo-dev: {error:#}");
            ExitCode::from(2)
        }
    }
}

fn run_cli() -> anyhow::Result<ExitCode> {
    let cli = Cli::parse();
    let cwd = std::env::current_dir()?;
    let repo_root = Git::discover(&cwd).context("not inside a git work tree")?;
    match cli.command {
        Command::Verify(args) => verify(&repo_root, *args),
        Command::Check { check } => match check {
            CheckCommand::Drift => check_drift(&repo_root),
        },
        Command::Hook { name } => match name.as_str() {
            "pre-commit" => {
                let git = Git::new(&repo_root);
                let branch = git.branch().unwrap_or_default();
                let allow = std::env::var("ALLOW_PROTECTED_COMMIT").ok();
                if let Some(lines) = hook::protected_branch_refusal(&branch, allow.as_deref()) {
                    for line in lines {
                        eprintln!("{line}");
                    }
                    return Ok(ExitCode::FAILURE);
                }
                let args = VerifyArgs {
                    fast: true,
                    staged: true,
                    failure_tail_lines: 120,
                    ..VerifyArgs::default()
                };
                verify(&repo_root, args)
            }
            _ => {
                if !hook::pushes_a_branch(std::io::stdin().lock()) {
                    return Ok(ExitCode::SUCCESS);
                }
                let args = VerifyArgs {
                    full: true,
                    failure_tail_lines: 120,
                    ..VerifyArgs::default()
                };
                verify(&repo_root, args)
            }
        },
    }
}

fn check_drift(repo_root: &std::path::Path) -> anyhow::Result<ExitCode> {
    let report = exo_dev::drift::check(repo_root)?;
    for v in &report.violations {
        let where_ = if v.line > 0 {
            format!("{}:{}", v.file, v.line)
        } else {
            v.file.clone()
        };
        eprintln!("  [{}] {where_}: {}", v.rule, v.message);
    }
    if report.violations.is_empty() {
        println!("check-drift: OK (Qt version, Qt setup, Qt SDK paths, no qmake project files)");
        Ok(ExitCode::SUCCESS)
    } else {
        println!("check-drift: FAILED");
        Ok(ExitCode::FAILURE)
    }
}

fn verify(repo_root: &std::path::Path, args: VerifyArgs) -> anyhow::Result<ExitCode> {
    let profile = if args.full {
        Profile::PrePush
    } else if let Some(name) = &args.profile {
        Profile::from_name(name).with_context(|| format!("unknown profile '{name}'"))?
    } else {
        Profile::PreCommit
    };
    let spec = profile.spec();
    let event = match (&args.event, spec.ci) {
        (Some(name), true) => Event::parse(name),
        (None, true) => bail!("profile '{}' is a CI job and needs --event", spec.name),
        (Some(_), false) => bail!("--event applies to CI profiles only"),
        (None, false) => Event::Local,
    };

    let git = Git::new(repo_root);
    let head = git.head();
    let dirty = git.dirty();
    let base = args
        .base
        .clone()
        .or_else(|| if spec.ci { None } else { git.default_base() });
    let scope = if spec.scoped {
        Scope::of(&git.changed_files(base.as_deref(), args.staged))
    } else {
        Scope::everything()
    };
    let jobs = args.jobs.filter(|j| *j > 0).unwrap_or_else(|| {
        host_lock::job_budget(std::env::var("EXOSNAP_VERIFY_JOBS").ok().as_deref())
    });
    let preset = args
        .preset
        .clone()
        .unwrap_or_else(|| spec.preset.to_string());
    let config = args
        .config
        .clone()
        .unwrap_or_else(|| spec.config.to_string());

    let plan = plan::build(&PlanInput {
        profile: spec,
        scope: &scope,
        event: event.clone(),
        base: base.clone(),
        staged: args.staged,
        packaging_changed: args.packaging_changed,
        windows: cfg!(windows),
        preset: preset.clone(),
        config: config.clone(),
    });

    let log_dir = repo_root.join(".workspace").join("verify");
    let mode = if spec.scoped { "Fast" } else { "Full" };
    let short_head = head.as_deref().map_or("?", |h| &h[..h.len().min(8)]);
    println!();
    println!(
        "exo-dev verify ({}) - HEAD {short_head}{}",
        spec.name,
        if dirty { " +dirty" } else { "" }
    );
    if spec.scoped {
        let categories = if scope.categories.is_empty() {
            "nothing".to_string()
        } else {
            scope.categories.join(", ")
        };
        println!(
            "  scope: {categories} ({} file(s))",
            scope.changed_files.len()
        );
        for reason in &scope.escalation_reasons {
            println!("  escalated: {reason}");
        }
    } else {
        println!("  scope: everything (this profile claims completeness)");
    }
    println!(
        "  jobs: {jobs} of {} cores (cmake --build --parallel, ctest -j, clang-tidy -j)",
        std::thread::available_parallelism().map_or(1, usize::from)
    );
    println!();

    let mut executor: Box<dyn Executor> = if args.dry_run {
        Box::new(DryRunExecutor::new(
            args.simulate_fail.clone(),
            log_dir.clone(),
        ))
    } else {
        let ctx = Context {
            repo_root: repo_root.to_path_buf(),
            profile: spec,
            event,
            base: base.clone(),
            staged: args.staged,
            preset,
            config,
            build_dir: plan.build_dir.clone(),
            jobs,
            pr_title: args.pr_title.clone(),
            pr_number: args.pr_number.clone(),
            tidy_cache_dir: args.tidy_cache_dir.clone(),
            head: head.clone(),
            dirty,
            runner: StepRunner {
                log_dir: log_dir.clone(),
                stream: spec.ci,
                failure_tail_lines: args.failure_tail_lines,
                env: Vec::new(),
            },
        };
        Box::new(RealExecutor::new(ctx, &plan))
    };

    let result = run::run(&plan, executor.as_mut());

    println!();
    for line in report::summary(&result) {
        println!("{line}");
    }
    let failures = report::failure_report(&result);
    if !failures.is_empty() {
        println!();
        println!("evidence:");
        for line in failures {
            println!("  {line}");
        }
    }
    if !result.tool_missing.is_empty() {
        println!();
        println!(
            "exo-dev verify ({}) could not run: {}. Install the missing tool(s): a gate that never \
             started is not a gate that passed.",
            spec.name,
            result.tool_missing.join(", ")
        );
    }

    let document = report::receipt(
        &result,
        &ReceiptFacts {
            profile: spec.name,
            mode,
            platform: std::env::consts::OS,
            head: head.as_deref().unwrap_or_default(),
            base: base.as_deref().unwrap_or_default(),
            dirty,
            scope: &scope,
        },
    );
    let result_path = args
        .result_path
        .unwrap_or_else(|| log_dir.join("latest.json"));
    report::save(&document, &result_path)
        .with_context(|| format!("could not write {}", result_path.display()))?;
    println!();
    println!("summary: {}", result_path.display());

    if result.passed {
        println!("exo-dev verify ({}): passed", spec.name);
        Ok(ExitCode::SUCCESS)
    } else {
        println!("exo-dev verify ({}): FAILED", spec.name);
        Ok(ExitCode::FAILURE)
    }
}
