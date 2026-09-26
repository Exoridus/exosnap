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
    /// Lint tooling that is not itself a pass/fail check over source, e.g.
    /// proving the checks a lint step relies on are still effective.
    Lint {
        #[command(subcommand)]
        command: LintCommand,
    },
}

#[derive(Subcommand)]
enum CheckCommand {
    /// The four build/Qt drift invariants: qt-version-consistency,
    /// setup-qt-centralized, no-qmake-project, qt-sdk-path-allowlist.
    Drift,
    /// Development provenance in source comments: task IDs, commit hashes,
    /// issue/PR numbers, private-workspace and machine-specific paths,
    /// conversation/agent narrative, branch names, non-ASCII punctuation.
    SourceHygiene {
        /// Commit to diff against. Defaults to HEAD (the working tree).
        #[arg(long, conflicts_with = "all")]
        base: Option<String>,
        /// Scan every tracked source file instead of only added/changed lines.
        #[arg(long)]
        all: bool,
        /// Restrict the run to one named rule.
        #[arg(long)]
        only: Option<String>,
    },
    /// Commit subjects locally, or a single subject (a pull request title or a
    /// merged subject) in CI.
    CommitPolicy {
        /// Check this single subject instead of the branch's commits.
        #[arg(long)]
        subject: Option<String>,
        /// The pull request `--subject` is the title of. Rejects a title that
        /// already ends in that number.
        #[arg(long)]
        pull_request_number: Option<u32>,
        /// Commit to diff and log against. Defaults to the merge base with
        /// origin/next, then origin/main, next, main.
        #[arg(long)]
        base: Option<String>,
        /// Restrict the run to one named rule (commit-subject or
        /// changelog-untouched).
        #[arg(long)]
        only: Option<String>,
        /// Require --subject to end in a pull request number: the
        /// merged-subject mode, for a line already on main.
        #[arg(long)]
        require_pull_request: bool,
    },
    /// clang-format over the tracked (or staged) C++ source in libs/, app/
    /// and tests/.
    Format {
        /// Scope to staged files instead of every tracked source file.
        #[arg(long)]
        staged: bool,
        /// Format in place. Combined with --staged, re-stages the formatted
        /// files and refuses a file that also has unstaged edits.
        #[arg(long)]
        fix: bool,
    },
}

#[derive(Subcommand)]
enum LintCommand {
    /// Runs clang-tidy against the one canary fixture per blocking check
    /// under scripts/tests/fixtures/lint-canaries, and fails if a check no
    /// longer fires on its own canary.
    Canaries {
        /// Explicit path to clang-tidy.exe. Autodetected from PATH when
        /// omitted.
        #[arg(long)]
        clang_tidy: Option<PathBuf>,
        /// Check only the named checks.
        #[arg(long, value_delimiter = ',')]
        only: Vec<String>,
    },
    /// Runs the curated, blocking clang-tidy check set
    /// (exo_dev::lint::canaries::BLOCKING_CHECKS) over the project's own
    /// sources.
    ClangTidy {
        /// Directory containing compile_commands.json. Defaults to the Ninja
        /// debug preset.
        #[arg(long, default_value = "build/windows-x64-ninja-debug")]
        build_dir: PathBuf,
        /// Git revision to diff against. When given, only the affected
        /// translation units are analysed. Omit for a full-tree pass.
        #[arg(long)]
        base: Option<String>,
        /// Explicit path to clang-tidy.exe. Autodetected from the Visual
        /// Studio LLVM toolset and then from PATH when omitted.
        #[arg(long)]
        clang_tidy: Option<PathBuf>,
        /// Parallel clang-tidy processes. Defaults to the processor count.
        #[arg(long, default_value_t = 0)]
        jobs: usize,
        /// Directory holding cached per-translation-unit results.
        #[arg(long)]
        cache_dir: Option<PathBuf>,
        /// Version the resolved clang-tidy must report, as a full version or
        /// a leading part of one ('22' matches 22.1.0). A mismatch fails the
        /// run.
        #[arg(long)]
        require_version: Option<String>,
        /// Print the blocking check list and exit.
        #[arg(long)]
        list_checks: bool,
    },
    /// Runs the advisory clang-tidy check set (`.clang-tidy` minus
    /// `clang-analyzer-*`) and cppcheck over the project's own sources.
    /// Distinct from `lint clang-tidy`: findings here are reported, never a
    /// gate for clang-tidy, while cppcheck fails the run on any finding.
    Quality {
        /// Restrict to one pass. Both run when omitted.
        #[arg(long, value_parser = ["cppcheck", "clang-tidy"])]
        only: Option<String>,
        /// Directory containing compile_commands.json for the clang-tidy
        /// pass. Defaults to the Ninja debug then release preset.
        #[arg(long)]
        build_dir: Option<PathBuf>,
        /// Restrict the clang-tidy pass to files changed since this
        /// revision. Full-tree when omitted.
        #[arg(long)]
        base: Option<String>,
        /// Parallel clang-tidy processes. Defaults to the processor count.
        #[arg(long, default_value_t = 0)]
        jobs: usize,
        /// Where to write the complete clang-tidy findings. The console only
        /// carries a summary.
        #[arg(long)]
        report_path: Option<PathBuf>,
    },
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
            CheckCommand::SourceHygiene { base, all, only } => {
                check_source_hygiene(&repo_root, base, all, only)
            }
            CheckCommand::CommitPolicy {
                subject,
                pull_request_number,
                base,
                only,
                require_pull_request,
            } => check_commit_policy(
                &repo_root,
                subject,
                pull_request_number,
                base,
                only,
                require_pull_request,
            ),
            CheckCommand::Format { staged, fix } => check_format(&repo_root, staged, fix),
        },
        Command::Lint { command } => match command {
            LintCommand::Canaries { clang_tidy, only } => {
                lint_canaries(&repo_root, clang_tidy, only)
            }
            LintCommand::ClangTidy {
                build_dir,
                base,
                clang_tidy,
                jobs,
                cache_dir,
                require_version,
                list_checks,
            } => lint_clang_tidy(
                &repo_root,
                build_dir,
                base,
                clang_tidy,
                jobs,
                cache_dir,
                require_version,
                list_checks,
            ),
            LintCommand::Quality {
                only,
                build_dir,
                base,
                jobs,
                report_path,
            } => lint_quality(&repo_root, only, build_dir, base, jobs, report_path),
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

fn check_source_hygiene(
    repo_root: &std::path::Path,
    base: Option<String>,
    all: bool,
    only: Option<String>,
) -> anyhow::Result<ExitCode> {
    let git = Git::new(repo_root);
    let scope = if all {
        exo_dev::source_hygiene::Scope::All
    } else {
        let base = base
            .or_else(|| git.default_base())
            .unwrap_or_else(|| "HEAD".to_string());
        exo_dev::source_hygiene::Scope::Diff { base }
    };
    let report = exo_dev::source_hygiene::check(repo_root, scope, only.as_deref())?;

    for finding in report.blocking() {
        eprintln!();
        eprintln!("source-hygiene: {}:{}", finding.file, finding.line);
        eprintln!("{} in source comment: \"{}\"", finding.rule, finding.value);
        eprintln!("{}", finding.fix);
    }
    let advisory_count = report.advisory().count();
    if advisory_count > 0 {
        println!();
        println!("source-hygiene ADVISORY: {advisory_count} finding(s)");
    }

    let blocking_count = report.blocking().count();
    if blocking_count == 0 {
        println!();
        let scope_desc = if all {
            "every tracked source file"
        } else {
            "the changed lines"
        };
        println!("source-hygiene: OK ({scope_desc})");
        Ok(ExitCode::SUCCESS)
    } else {
        println!();
        println!("{blocking_count} blocking finding(s).");
        Ok(ExitCode::FAILURE)
    }
}

fn check_commit_policy(
    repo_root: &std::path::Path,
    subject: Option<String>,
    pull_request_number: Option<u32>,
    base: Option<String>,
    only: Option<String>,
    require_pull_request: bool,
) -> anyhow::Result<ExitCode> {
    let changelog_cut = std::env::var("EXOSNAP_CHANGELOG_CUT").as_deref() == Ok("1");
    let request = exo_dev::commit_policy::CheckRequest {
        subject: subject.as_deref(),
        pull_request_number,
        require_pull_request,
        base: base.as_deref(),
        only: only.as_deref(),
        changelog_cut,
    };
    let report = exo_dev::commit_policy::check(
        repo_root,
        &request,
        &exo_dev::commit_policy::CommitPolicyOptions::default(),
    )?;
    print!("{}", exo_dev::commit_policy::render(&report));
    if report.ok() {
        Ok(ExitCode::SUCCESS)
    } else {
        Ok(ExitCode::FAILURE)
    }
}

fn check_format(repo_root: &std::path::Path, staged: bool, fix: bool) -> anyhow::Result<ExitCode> {
    let report = exo_dev::lint::format::format(repo_root, staged, fix)?;
    print!("{}", report.output);

    if report.files.is_empty() {
        println!("clang-format: SKIP (no {} C++ source files)", report.scope);
        return Ok(ExitCode::SUCCESS);
    }
    if report.fixed {
        println!(
            "clang-format: OK (formatted {} file(s))",
            report.files.len()
        );
        return Ok(ExitCode::SUCCESS);
    }
    if report.violations {
        eprintln!("clang-format violations found. Fix with: clang-format -i <file>");
        return Ok(ExitCode::FAILURE);
    }
    println!("clang-format: OK");
    Ok(ExitCode::SUCCESS)
}

fn lint_canaries(
    repo_root: &std::path::Path,
    clang_tidy: Option<PathBuf>,
    only: Vec<String>,
) -> anyhow::Result<ExitCode> {
    let report = exo_dev::lint::canaries::run_canaries(repo_root, clang_tidy.as_deref(), &only)?;
    for check in exo_dev::lint::canaries::BLOCKING_CHECKS {
        let in_scope = only.is_empty() || only.iter().any(|wanted| wanted == check);
        let failed = report
            .failures
            .iter()
            .any(|failure| failure.check == *check);
        if in_scope && !failed {
            println!("  fires  {check}");
        }
    }
    println!();
    println!("lint canaries: {} check(s) exercised", report.checked);
    if report.ok() {
        return Ok(ExitCode::SUCCESS);
    }
    println!();
    for failure in &report.failures {
        eprintln!("FAIL  {} : {}", failure.check, failure.detail);
    }
    println!();
    println!(
        "docs/dev/static-analysis.md explains why a silent check and a clean tree look the same."
    );
    Ok(ExitCode::FAILURE)
}

#[allow(clippy::too_many_arguments)]
fn lint_clang_tidy(
    repo_root: &std::path::Path,
    build_dir: PathBuf,
    base: Option<String>,
    clang_tidy: Option<PathBuf>,
    jobs: usize,
    cache_dir: Option<PathBuf>,
    require_version: Option<String>,
    list_checks: bool,
) -> anyhow::Result<ExitCode> {
    let build_dir = if build_dir.is_absolute() {
        build_dir
    } else {
        repo_root.join(build_dir)
    };
    let cache_dir =
        cache_dir.unwrap_or_else(|| exo_dev::evidence::tool_cache_dir("clang-tidy", &[], None));

    let report = exo_dev::lint::clang_tidy::run_blocking(
        repo_root,
        &build_dir,
        base.as_deref(),
        &cache_dir,
        jobs,
        require_version.as_deref(),
        list_checks,
        clang_tidy.as_deref(),
    )?;
    if list_checks {
        return Ok(ExitCode::SUCCESS);
    }

    println!("clang-tidy      : {}", report.tool_path.display());
    if !report.compile_db.as_os_str().is_empty() {
        println!("compile database: {}", report.compile_db.display());
    }
    println!(
        "blocking checks : {}",
        exo_dev::lint::canaries::BLOCKING_CHECKS.join(", ")
    );
    println!(
        "scope           : {}, {} translation unit(s), {jobs} parallel job(s)",
        report.scope, report.analyzed
    );
    if report.cache_enabled {
        println!(
            "result cache    : {} - {} replayed, {} analysed",
            report.cache_dir.display(),
            report.replayed,
            report.analyzed.saturating_sub(report.replayed)
        );
    }
    println!();

    if report.ok() {
        println!(
            "clang-tidy blocking check set: clean ({} translation unit(s)).",
            report.analyzed
        );
        return Ok(ExitCode::SUCCESS);
    }

    eprintln!(
        "clang-tidy blocking check violations: {}",
        report.violations.len()
    );
    for violation in &report.violations {
        eprintln!("  {violation}");
    }
    eprintln!();
    eprintln!(
        "These checks are required to stay at zero findings. Fix the code, or take the check out \
         of BLOCKING_CHECKS in tools/exo-dev/src/lint/canaries.rs and out of .clang-tidy."
    );
    Ok(ExitCode::FAILURE)
}

fn lint_quality(
    repo_root: &std::path::Path,
    only: Option<String>,
    build_dir: Option<PathBuf>,
    base: Option<String>,
    jobs: usize,
    report_path: Option<PathBuf>,
) -> anyhow::Result<ExitCode> {
    let only = match only.as_deref() {
        None => None,
        Some("cppcheck") => Some(exo_dev::lint::quality::Only::CppCheck),
        Some("clang-tidy") => Some(exo_dev::lint::quality::Only::ClangTidy),
        Some(other) => bail!("unknown --only '{other}'"),
    };

    let report = match exo_dev::lint::quality::run(
        repo_root,
        build_dir.as_deref(),
        base.as_deref(),
        only,
        jobs,
        report_path.as_deref(),
    ) {
        Ok(report) => report,
        // Distinct from every other exit code, matching check-quality.ps1's
        // original ToolMissingExitCode: a caller must be able to tell "the
        // tool this run needed is not installed" from "the checks ran and
        // found something" and from any other failure to launch.
        Err(error) => match error.downcast::<exo_dev::executor::ToolMissing>() {
            Ok(missing) => {
                eprintln!();
                eprintln!("Static quality check INCOMPLETE: {missing}.");
                return Ok(ExitCode::from(3));
            }
            Err(error) => return Err(error),
        },
    };

    if let Some(clang_tidy) = &report.clang_tidy {
        println!(
            "clang-tidy ADVISORY: {} translation unit(s) ({}) in {} batch(es), {} parallel job(s)",
            clang_tidy.analyzed, clang_tidy.scope, clang_tidy.batches, clang_tidy.jobs
        );
        if clang_tidy.findings.is_empty() {
            println!("clang-tidy: OK");
        } else {
            for finding in &clang_tidy.findings {
                println!("{finding}");
            }
        }
    } else if let Some(reason) = &report.clang_tidy_skip_reason {
        println!("clang-tidy: SKIP ({reason})");
    }

    let mut ok = true;
    if let Some(cppcheck) = &report.cppcheck {
        print!("{}", cppcheck.output);
        if cppcheck.ok {
            println!("cppcheck: OK");
        } else {
            println!("cppcheck: FAILED");
            ok = false;
        }
    }

    println!();
    if ok {
        println!("Static quality check passed.");
        Ok(ExitCode::SUCCESS)
    } else {
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
