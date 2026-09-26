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
    /// Build one CMake tree and run its CTest suite in an isolated
    /// environment: a throwaway EXOSNAP_CONFIG_DIR, offscreen Qt and the
    /// canonical Qt on PATH. The full ctest output goes to
    /// <build-dir>/Testing/last-run.log and the verdict to
    /// <build-dir>/Testing/last-run-receipt.json, whose `reusable` field is
    /// the one to read. Exit codes: 0 the selected tests passed and the run
    /// is valid, 2 the build directory does not exist, 3 the tree was not
    /// proven to match the source, 4 not a valid verification run, and
    /// otherwise the build's or ctest's own exit code.
    Test(TestArgs),
    /// Pull request lifecycle: open a draft with a validated title, then
    /// merge with the squash subject the changelog cut reads.
    Pr {
        #[command(subcommand)]
        pr: PrCommand,
    },
    /// Guards for the privacy promises in PRIVACY.md and docs/product-spec.md.
    Privacy {
        #[command(subcommand)]
        privacy: PrivacyCommand,
    },
}

#[derive(Subcommand)]
enum PrCommand {
    /// Opens the pull request for the current branch as a draft, reads its
    /// stored title back, and marks it ready once it re-validates.
    Open {
        /// The pull request title. Defaults to the subject of the newest
        /// commit on this branch that is not on --base.
        #[arg(long)]
        subject: Option<String>,
        /// The pull request description. Defaults to a placeholder the
        /// author is expected to replace.
        #[arg(long)]
        body: Option<String>,
        /// Read the description from this file instead of --body.
        #[arg(long)]
        body_file: Option<PathBuf>,
        /// Base branch.
        #[arg(long, default_value = "next")]
        base: String,
        /// Leave the pull request in draft instead of marking it ready.
        #[arg(long)]
        keep_draft: bool,
        /// Do not push the branch first. Fails if the branch has no
        /// upstream.
        #[arg(long)]
        no_push: bool,
    },
    /// Squash-merges a pull request with the exact subject the changelog cut
    /// reads. Without --confirm, prints the subject and merges nothing.
    Merge {
        /// The pull request to merge. Defaults to the one for the current
        /// branch.
        #[arg(long)]
        number: Option<u32>,
        /// Required to actually merge.
        #[arg(long)]
        confirm: bool,
        /// Delete the head branch after the merge.
        #[arg(long)]
        delete_branch: bool,
        /// Enable auto-merge instead of merging now.
        #[arg(long)]
        auto: bool,
    },
}

#[derive(Args)]
struct TestArgs {
    /// CMake build tree to test, relative to the repository root. The tree
    /// `exo-dev verify` configures and builds by default, so the inner loop
    /// and the gate judge the same binaries.
    #[arg(long, default_value = "build/windows-x64-ninja-debug")]
    build_dir: PathBuf,
    /// Multi-config configuration to run (ctest -C).
    #[arg(long, default_value = "Debug")]
    config: String,
    /// Regex passed to ctest -R to select test binaries by name, e.g.
    /// "recorder_core.".
    #[arg(long, default_value = "")]
    filter: String,
    /// Label excluded from the run, e.g. "live" for the binaries that query
    /// real hardware. A plain word is anchored to ^word$, because ctest -LE
    /// is a regex and "live" would also drop "live_verify". A value with
    /// regex metacharacters passes through unchanged.
    #[arg(long, default_value = "")]
    exclude_label: String,
    /// Run only the tests of one execution phase: hermetic, cpu, gpu,
    /// desktop, vm or human.
    #[arg(long, value_parser = ["hermetic", "cpu", "gpu", "desktop", "vm", "human"])]
    phase: Option<String>,
    /// Parallel test jobs (ctest -j). Defaults to EXOSNAP_VERIFY_JOBS, then
    /// all cores but two.
    #[arg(long, default_value_t = 0)]
    jobs: usize,
    /// Skip the build and test what the tree holds. Only a Ninja tree can
    /// then be proven current, so without --allow-stale this usually ends in
    /// exit 3.
    #[arg(long)]
    no_build: bool,
    /// Report a result although the tree was not proven to match the source
    /// (instead of exit 3). The result may describe old binaries.
    #[arg(long, requires = "no_build")]
    allow_stale: bool,
}

#[derive(Subcommand)]
enum PrivacyCommand {
    /// A network primitive or disallowed http(s) host literal outside the
    /// known GitHub/Sentry call sites, under app/, libs/, apps/.
    NetworkEgress,
    /// The crash-report tag allowlist (crash_scrubber.h) against PRIVACY.md
    /// and docs/product-spec.md.
    Allowlist,
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
    /// app/cli/CommandLineFlags.cpp against the parser sources, the
    /// acceptance-harness script and exo-verify's own launch calls.
    CliFlags,
    /// The rulesets this repository declares under .github/rulesets against
    /// the ones GitHub actually enforces. Never writes.
    Rulesets {
        /// Directory holding the intended ruleset payloads. Defaults to
        /// .github/rulesets under the repository root.
        #[arg(long)]
        desired: Option<PathBuf>,
        /// Read the live state from this file instead of the GitHub API: the
        /// array `gh api repos/:owner/:repo/rulesets` returns, with each
        /// entry's rules and bypass_actors expanded. Usable offline.
        #[arg(long)]
        current_json: Option<PathBuf>,
        /// Print only the difference, not the full comparison.
        #[arg(long)]
        quiet: bool,
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
            CheckCommand::CliFlags => check_cli_flags(&repo_root),
            CheckCommand::Rulesets {
                desired,
                current_json,
                quiet,
            } => check_rulesets(&repo_root, desired, current_json, quiet),
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
        Command::Test(args) => test(&repo_root, args),
        Command::Pr { pr } => match pr {
            PrCommand::Open {
                subject,
                body,
                body_file,
                base,
                keep_draft,
                no_push,
            } => pr_open(
                &repo_root, subject, body, body_file, base, keep_draft, no_push,
            ),
            PrCommand::Merge {
                number,
                confirm,
                delete_branch,
                auto,
            } => pr_merge(&repo_root, number, confirm, delete_branch, auto),
        },
        Command::Privacy { privacy } => match privacy {
            PrivacyCommand::NetworkEgress => privacy_network_egress(&repo_root),
            PrivacyCommand::Allowlist => privacy_allowlist(&repo_root),
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

fn test(repo_root: &std::path::Path, args: TestArgs) -> anyhow::Result<ExitCode> {
    use exo_dev::test::{
        catalog::Phase,
        host::{Console, RealHost},
        runner,
    };

    let options = runner::Options {
        build_dir: args.build_dir,
        config: args.config,
        filter: args.filter,
        exclude_label: args.exclude_label,
        phase: args.phase.as_deref().and_then(Phase::from_name),
        jobs: args.jobs,
        no_build: args.no_build,
        allow_stale: args.allow_stale,
        ..runner::Options::new(repo_root)
    };
    let result = runner::run(&options, &RealHost::default(), &mut Console::live());
    Ok(exit_code(result.exit_code))
}

/// A child's exit code passed through unchanged. Codes outside 0..=255 (a
/// Windows NTSTATUS from a crashed ctest) cannot be an `ExitCode`, so those
/// leave through `process::exit`.
fn exit_code(code: i32) -> ExitCode {
    match u8::try_from(code) {
        Ok(code) => ExitCode::from(code),
        Err(_) => {
            use std::io::Write as _;
            let _ = std::io::stdout().flush();
            std::process::exit(code)
        }
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

/// Exit codes: 0 clean, 1 an unregistered or duplicate flag was found, 2 when
/// the registry or a required parser source is missing or unreadable. The
/// exit-2 case is not handled here: `check` returns it as an `Err`, which
/// propagates out of `run_cli` and reaches the generic exit-2 handler in
/// `main`, since a missing source needs the list in `cli_flags.rs` updated
/// rather than a report field.
fn check_cli_flags(repo_root: &std::path::Path) -> anyhow::Result<ExitCode> {
    let report = exo_dev::cli_flags::check(repo_root)?;
    print!("{}", exo_dev::cli_flags::render(&report));
    if report.ok() {
        Ok(ExitCode::SUCCESS)
    } else {
        Ok(ExitCode::FAILURE)
    }
}

/// Exit codes match the porting contract exactly: 0 clean, 1 drift found, 2
/// when the declared or live state could not be established at all. The
/// unreadable case is mapped here from `CheckOutcome::Unreadable` rather than
/// left to propagate as an error, so it stays a deliberate exit code and
/// never an accident of the generic error handler.
fn check_rulesets(
    repo_root: &std::path::Path,
    desired: Option<PathBuf>,
    current_json: Option<PathBuf>,
    quiet: bool,
) -> anyhow::Result<ExitCode> {
    let declared_dir = desired.unwrap_or_else(|| repo_root.join(".github/rulesets"));
    let live = match current_json {
        Some(path) => exo_dev::rulesets::LiveSource::File(path),
        None => exo_dev::rulesets::LiveSource::GhApi,
    };
    match exo_dev::rulesets::check(&declared_dir, live)? {
        exo_dev::rulesets::CheckOutcome::Unreadable(message) => {
            eprintln!("{message}");
            Ok(ExitCode::from(2))
        }
        exo_dev::rulesets::CheckOutcome::Compared(report) => {
            print!("{}", exo_dev::rulesets::render(&report, quiet));
            if report.ok() {
                Ok(ExitCode::SUCCESS)
            } else {
                Ok(ExitCode::FAILURE)
            }
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn pr_open(
    repo_root: &std::path::Path,
    subject: Option<String>,
    body: Option<String>,
    body_file: Option<PathBuf>,
    base: String,
    keep_draft: bool,
    no_push: bool,
) -> anyhow::Result<ExitCode> {
    let request = exo_dev::pr::OpenRequest {
        subject,
        body,
        body_file,
        base,
        keep_draft,
        no_push,
    };
    let outcome = exo_dev::pr::open(repo_root, &exo_dev::pr::RealGh::new(), &request)?;
    print!("{}", exo_dev::pr::render_open(&outcome));
    Ok(ExitCode::SUCCESS)
}

fn pr_merge(
    repo_root: &std::path::Path,
    number: Option<u32>,
    confirm: bool,
    delete_branch: bool,
    auto: bool,
) -> anyhow::Result<ExitCode> {
    let request = exo_dev::pr::MergeRequest {
        number,
        confirm,
        delete_branch,
        auto,
    };
    let outcome = exo_dev::pr::merge(repo_root, &exo_dev::pr::RealGh::new(), &request)?;
    print!("{}", exo_dev::pr::render_merge(&outcome));
    Ok(ExitCode::SUCCESS)
}
fn privacy_network_egress(repo_root: &std::path::Path) -> anyhow::Result<ExitCode> {
    let report = exo_dev::privacy::network_egress::check_network_egress(repo_root)?;
    print!("{}", exo_dev::privacy::network_egress::render(&report));
    if report.ok() {
        Ok(ExitCode::SUCCESS)
    } else {
        Ok(ExitCode::FAILURE)
    }
}

fn privacy_allowlist(repo_root: &std::path::Path) -> anyhow::Result<ExitCode> {
    let report = exo_dev::privacy::allowlist::check_allowlist(repo_root)?;
    print!("{}", exo_dev::privacy::allowlist::render(&report));
    if report.ok() {
        Ok(ExitCode::SUCCESS)
    } else {
        Ok(ExitCode::FAILURE)
    }
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

/// `error`'s exit code for `lint quality`: `ToolMissing` becomes exit 3,
/// distinct from every other exit code. A caller must be able to tell "the
/// tool this run needed is not installed" from "the checks ran and found
/// something" and from any other failure to launch. Any other error is
/// handed back unchanged, for `run_cli` to report and turn into exit 2.
fn quality_tool_missing_exit(error: anyhow::Error) -> anyhow::Result<ExitCode> {
    match error.downcast::<exo_dev::executor::ToolMissing>() {
        Ok(missing) => {
            eprintln!();
            eprintln!("Static quality check INCOMPLETE: {missing}.");
            Ok(ExitCode::from(3))
        }
        Err(error) => Err(error),
    }
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
        None,
    ) {
        Ok(report) => report,
        Err(error) => return quality_tool_missing_exit(error),
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_tool_missing_error_becomes_exit_code_3() {
        let error: anyhow::Error =
            exo_dev::executor::ToolMissing("cppcheck, clang-tidy not installed".into()).into();
        let code = quality_tool_missing_exit(error).unwrap();
        // std::process::ExitCode has no PartialEq. Debug is stable and
        // exact enough to tell 3 apart from every other code this function
        // returns.
        assert_eq!(format!("{code:?}"), format!("{:?}", ExitCode::from(3)));
    }

    #[test]
    fn any_other_error_is_handed_back_unchanged() {
        let error = anyhow::anyhow!("boom");
        let result = quality_tool_missing_exit(error);
        assert!(result.is_err());
        assert_eq!(result.unwrap_err().to_string(), "boom");
    }
}
