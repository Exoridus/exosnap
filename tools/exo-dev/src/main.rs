use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context as _, bail};
use clap::{Args, Parser, Subcommand};

use exo_dev::benchmark::run::Frontend;
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
    /// Standalone packaging manifest validators, also reachable individually
    /// outside a verify profile.
    Packaging {
        #[command(subcommand)]
        packaging: PackagingCommand,
    },
    /// Release preparation: version bump, changelog assembly, release notes,
    /// and the packaging publication drift check.
    Release {
        #[command(subcommand)]
        release: ReleaseCommand,
    },
    /// Regenerates the detached-signature fixture pasted into
    /// libs/update/tests/test_update_signature.cpp and prints it.
    #[cfg(feature = "dev-tools")]
    GenManifestFixture,
    /// Measures A/V clock drift of a recorded clapper file. Exit codes: 0
    /// drift within budget, 2 drift over budget, 3 could not measure.
    AvSyncCheck(AvSyncArgs),
    /// Regenerates the golden clapper clip: synchronized flash and beep
    /// markers on one synthetic timeline, at ~0 drift on purpose.
    GenAvSyncFixture(FixtureArgs),
    /// Recomputes encode-latency and frame-time percentiles from an
    /// engine.jsonl perf log, and, given a second log, a before/after delta.
    PerfAnalyze(PerfAnalyzeArgs),
    /// Sweeps NVENC preset/rate-control combinations through
    /// probe_encode_file, scores each encode with ffmpeg/libvmaf and reports
    /// BD-rate across the sweep. Dev-only: needs real NVENC hardware and a
    /// local ffmpeg build with libvmaf, never run in CI.
    EncoderQualityMatrix(exo_dev::encoder_quality_matrix::MatrixArgs),
    /// Reads a crash minidump and resolves the faulting instruction, the
    /// crashed thread's stack scan and every module's PDB identity.
    ReadCrashDump(CrashDumpArgs),
    /// The frontend recording benchmark: display topology, one measured run,
    /// an alternating campaign, cross-run comparison and Superposition scene
    /// calibration.
    Benchmark {
        #[command(subcommand)]
        benchmark: BenchmarkCommand,
    },
    /// Regenerates exosnap-app.ico, the thumbnail-toolbar glyph .ico files
    /// and exosnap-logo.svg from the canonical mark geometry.
    #[cfg(feature = "dev-tools")]
    GenerateAppIcons,
    /// Regenerates the ExoSnap mark suite from parameters.json.
    #[cfg(feature = "dev-tools")]
    GenerateBrandMarks {
        /// Report drift instead of writing; exits non-zero if the suite is
        /// stale.
        #[arg(long)]
        check: bool,
    },
}

#[derive(Args)]
struct AvSyncArgs {
    /// Recorded clapper file (mkv/mp4/webm).
    file: PathBuf,
    /// Flash detection threshold as a fraction of the luma range.
    #[arg(long, default_value_t = 0.7)]
    luma_threshold_frac: f64,
    /// Beep detection threshold as a fraction of the RMS range.
    #[arg(long, default_value_t = 0.5)]
    rms_threshold_frac: f64,
    /// Total drift budget over the measured span, ms (advisory default 20).
    #[arg(long, default_value_t = 20.0)]
    max_drift_ms: f64,
    /// Alternative drift budget as a rate (ms/hour); overrides
    /// --max-drift-ms when set.
    #[arg(long)]
    max_drift_ms_per_hour: Option<f64>,
    /// Report a verdict even when the clapper signal does not qualify to
    /// carry one (diagnostic; the numbers are printed either way).
    #[arg(long)]
    unqualified_reference: bool,
    /// Require and select exactly this many scheduled marker pairs.
    #[arg(long)]
    expected_markers: Option<usize>,
    /// Expected comma-separated marker schedule, e.g. 10,3600,7190.
    #[arg(long, value_delimiter = ',')]
    marker_times_seconds: Option<Vec<f64>>,
    /// Maximum flash/beep edge separation when pairing markers.
    #[arg(long, default_value_t = 250.0)]
    max_pair_skew_ms: f64,
    /// Maximum interval error against --marker-times-seconds.
    #[arg(long, default_value_t = 2.0)]
    schedule_tolerance_seconds: f64,
    /// Emit the full measurement as JSON.
    #[arg(long)]
    json: bool,
}

#[derive(Args)]
struct FixtureArgs {
    /// Clip length in seconds.
    #[arg(long, default_value_t = 4.0)]
    duration: f64,
    /// Marker count (at least 2).
    #[arg(long, default_value_t = 5)]
    markers: usize,
    /// Output path. Defaults to tests/samples/media/clapper-golden.mp4.
    #[arg(long)]
    out: Option<PathBuf>,
}

#[derive(Subcommand)]
enum ReleaseCommand {
    /// Moves the product version across every packaging surface that
    /// repeats it by hand, resets the values only a release can produce to
    /// their placeholders, and self-verifies the result.
    BumpVersion {
        /// The new product version, x.y.z. A prerelease suffix is refused: it
        /// names a release identity, not a product version.
        #[arg(long)]
        version: String,
        /// Bump a dirty tree anyway. Without it, a bump refuses so the whole
        /// diff is the bump and nothing else.
        #[arg(long)]
        force: bool,
        /// Repository root to bump. Defaults to the current repository.
        #[arg(long)]
        repo_root: Option<PathBuf>,
    },
    /// Assembles the changelog section for everything merged since the last
    /// released version tag, and prints it or writes it into CHANGELOG.md.
    Changelog {
        /// Read from this ref instead of the last released version tag.
        #[arg(long)]
        since: Option<String>,
        /// Read up to this ref. Defaults to origin/main, falling back to HEAD.
        #[arg(long)]
        until: Option<String>,
        /// Render the section as a released version, headed 'x.y.z - date',
        /// instead of 'Unreleased'.
        #[arg(long)]
        version: Option<String>,
        /// The release date for --version. Defaults to today, UTC.
        #[arg(long)]
        date: Option<String>,
        /// Write the rendered section into CHANGELOG.md instead of printing
        /// it. With --version the Unreleased section is replaced by the
        /// release section and a new empty Unreleased is opened above it.
        #[arg(long)]
        apply: bool,
    },
    /// Renders a release's notes from a template and the changelog.
    ReleaseNotes {
        /// The full release identity, as in 0.9.1 or 0.9.1-rc3. The
        /// changelog section is looked up under the x.y.z part.
        #[arg(long)]
        version: String,
        /// The git tag. Defaults to 'v' + version.
        #[arg(long)]
        tag: Option<String>,
        /// The tag the compare link starts at. Defaults to the newest
        /// version tag below this one that is an ancestor of HEAD.
        #[arg(long)]
        previous_tag: Option<String>,
        /// The commit the artifacts were built from. Defaults to HEAD.
        #[arg(long)]
        commit: Option<String>,
        /// Render the release-candidate template.
        #[arg(long)]
        candidate: bool,
        /// Base URL for the compare and releases links. Defaults to the
        /// ExoSnap repository.
        #[arg(long)]
        repository_url: Option<String>,
        /// Write the notes here instead of to standard output.
        #[arg(long)]
        out_file: Option<PathBuf>,
    },
    /// Reads packaging/publication-policy.json against the packaging tree,
    /// and, unless --offline, polls each channel's public feed. The feed
    /// half is advisory only: it never fails the run.
    FeedDrift {
        /// Check only the policy. No network.
        #[arg(long)]
        offline: bool,
        /// The policy to check. Defaults to packaging/publication-policy.json.
        #[arg(long)]
        policy_path: Option<PathBuf>,
        /// Per-feed request timeout.
        #[arg(long, default_value_t = 20)]
        timeout_seconds: u64,
    },
}

#[derive(Args)]
struct PerfAnalyzeArgs {
    /// Path to engine.jsonl (or any file with perf records).
    engine_jsonl: PathBuf,
    /// Optional second log for a before/after delta.
    compare_jsonl: Option<PathBuf>,
    /// Emit machine-readable JSON instead of a table.
    #[arg(long)]
    json: bool,
}

#[derive(Args)]
struct CrashDumpArgs {
    /// The .dmp file to read.
    dump: PathBuf,
    /// Directory to try, ahead of a module's own recorded path, when
    /// locating the on-disk binary to verify its PDB identity against.
    #[arg(long, default_value = "")]
    pdb_search_path: PathBuf,
}

#[derive(Subcommand)]
enum BenchmarkCommand {
    /// Asserts the scenario's expected capture/UI displays against the
    /// machine, without running anything.
    Topology {
        /// Scenario name under tools/benchmark/scenarios.
        #[arg(long)]
        scenario: String,
    },
    /// Runs one measurement: verify topology, start the workload, launch
    /// ExoSnap with --auto-record, wait, collect artifacts and apply
    /// acceptance criteria.
    Run {
        #[arg(long, value_parser = ["widgets", "quick"])]
        frontend: String,
        #[arg(long)]
        scenario: String,
        #[arg(long, default_value_t = 1)]
        run_index: u32,
        #[arg(long)]
        output_root: Option<PathBuf>,
        /// Explicit Widgets executable. Required to measure the removed
        /// Widgets frontend; there is no default.
        #[arg(long)]
        widgets_exe: Option<PathBuf>,
        #[arg(long)]
        quick_exe: Option<PathBuf>,
        #[arg(long)]
        superposition_cli: Option<PathBuf>,
        /// A disposable validation run: allows a Debug binary, excluded from
        /// campaign statistics.
        #[arg(long)]
        calibration: bool,
        /// Escape hatch for working on the tooling without the target
        /// displays attached. An accepted run must never use it.
        #[arg(long)]
        skip_topology_check: bool,
    },
    /// Runs the alternating frontend campaign for one scenario.
    Campaign {
        #[arg(long)]
        scenario: String,
        /// Comma-separated frontends (widgets, quick). Defaults to six Quick
        /// runs: the Widgets frontend was removed with the Qt Quick cutover.
        #[arg(long, value_delimiter = ',')]
        order: Vec<String>,
        #[arg(long, default_value_t = 20)]
        cooldown_seconds: u64,
        #[arg(long)]
        output_root: Option<PathBuf>,
        #[arg(long)]
        widgets_exe: Option<PathBuf>,
        #[arg(long)]
        quick_exe: Option<PathBuf>,
        #[arg(long)]
        superposition_cli: Option<PathBuf>,
        #[arg(long)]
        calibration: bool,
    },
    /// Builds the cross-run comparison dataset for a set of accepted runs.
    Compare {
        /// One or more run directories (each holding run.json).
        #[arg(long = "run-dir", required = true)]
        run_dirs: Vec<PathBuf>,
        /// Where to write comparison.json. Defaults beside the first run
        /// directory's parent.
        #[arg(long)]
        report_path: Option<PathBuf>,
    },
    /// Surveys candidate Superposition scenes without ExoSnap running, and
    /// ranks them by FPS variability.
    SceneSurvey {
        /// Scene numbers to survey (e.g. --scene 4 --scene 5).
        #[arg(long = "scene")]
        scenes: Vec<u32>,
        #[arg(long)]
        superposition_cli: PathBuf,
        #[arg(
            long,
            default_value = ".workspace/benchmark-results/calibration/scene-survey"
        )]
        output_root: PathBuf,
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
    /// Chocolatey, WinGet and Scoop against the canonical CMake project
    /// version (or an explicit one). The Chocolatey call is always
    /// version-only.
    PackagingVersion {
        /// Target version. Defaults to the canonical CMake project version.
        #[arg(long)]
        version: Option<String>,
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

#[derive(Subcommand)]
enum PackagingCommand {
    /// packaging/chocolatey/ against the target version, and, unless
    /// --version-only, the mechanically checkable Chocolatey moderation
    /// subset.
    Chocolatey {
        /// Target version. Defaults to the canonical CMake project version.
        #[arg(long)]
        version: Option<String>,
        /// Skip the checksum64-vs-manifest cross-check and the moderation
        /// subset entirely: the version axis only, for a pull-request gate
        /// run before a release exists.
        #[arg(long)]
        version_only: bool,
        /// Release artifact manifest to check checksum64 against. Defaults
        /// to the local release build's own artifact manifest for that
        /// version.
        #[arg(long)]
        manifest_path: Option<PathBuf>,
        /// Fail instead of skipping when no release artifact manifest is
        /// found. Ignored with --version-only.
        #[arg(long)]
        require_manifest: bool,
    },
    /// The canonical Codexo.ExoSnap WinGet manifest set against the target
    /// version.
    Winget {
        /// Target version. Defaults to the canonical CMake project version.
        #[arg(long)]
        version: Option<String>,
    },
    /// packaging/scoop/exosnap.json against the target version.
    Scoop {
        /// Target version. Defaults to the canonical CMake project version.
        #[arg(long)]
        version: Option<String>,
    },
    /// packaging/msi/Package.wxs stays metadata-only and references the
    /// auto-generated harvest component group.
    MsiHarvest,
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
            CheckCommand::PackagingVersion { version } => {
                check_packaging_version(&repo_root, version)
            }
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
        Command::Packaging { packaging } => match packaging {
            PackagingCommand::Chocolatey {
                version,
                version_only,
                manifest_path,
                require_manifest,
            } => packaging_chocolatey(
                &repo_root,
                version,
                version_only,
                manifest_path,
                require_manifest,
            ),
            PackagingCommand::Winget { version } => packaging_winget(&repo_root, version),
            PackagingCommand::Scoop { version } => packaging_scoop(&repo_root, version),
            PackagingCommand::MsiHarvest => packaging_msi_harvest(&repo_root),
        },
        Command::Release { release } => match release {
            ReleaseCommand::BumpVersion {
                version,
                force,
                repo_root: override_root,
            } => release_bump_version(
                override_root.as_deref().unwrap_or(&repo_root),
                &version,
                force,
            ),
            ReleaseCommand::Changelog {
                since,
                until,
                version,
                date,
                apply,
            } => release_changelog(&repo_root, since, until, version, date, apply),
            ReleaseCommand::ReleaseNotes {
                version,
                tag,
                previous_tag,
                commit,
                candidate,
                repository_url,
                out_file,
            } => release_release_notes(
                &repo_root,
                version,
                tag,
                previous_tag,
                commit,
                candidate,
                repository_url,
                out_file,
            ),
            ReleaseCommand::FeedDrift {
                offline,
                policy_path,
                timeout_seconds,
            } => release_feed_drift(&repo_root, offline, policy_path, timeout_seconds),
        },
        Command::PerfAnalyze(args) => perf_analyze(args),
        Command::EncoderQualityMatrix(args) => exo_dev::encoder_quality_matrix::run(&args),
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
        #[cfg(feature = "dev-tools")]
        Command::GenManifestFixture => gen_manifest_fixture(),
        Command::AvSyncCheck(args) => av_sync_check(args),
        Command::GenAvSyncFixture(args) => gen_av_sync_fixture(&repo_root, args),
        Command::ReadCrashDump(args) => read_crash_dump(args),
        Command::Benchmark { benchmark } => match benchmark {
            BenchmarkCommand::Topology { scenario } => benchmark_topology(&repo_root, &scenario),
            BenchmarkCommand::Run {
                frontend,
                scenario,
                run_index,
                output_root,
                widgets_exe,
                quick_exe,
                superposition_cli,
                calibration,
                skip_topology_check,
            } => benchmark_run(
                &repo_root,
                &frontend,
                &scenario,
                run_index,
                output_root,
                widgets_exe,
                quick_exe,
                superposition_cli,
                calibration,
                skip_topology_check,
            ),
            BenchmarkCommand::Campaign {
                scenario,
                order,
                cooldown_seconds,
                output_root,
                widgets_exe,
                quick_exe,
                superposition_cli,
                calibration,
            } => benchmark_campaign(
                &repo_root,
                &scenario,
                order,
                cooldown_seconds,
                output_root,
                widgets_exe,
                quick_exe,
                superposition_cli,
                calibration,
            ),
            BenchmarkCommand::Compare {
                run_dirs,
                report_path,
            } => benchmark_compare(run_dirs, report_path),
            BenchmarkCommand::SceneSurvey {
                scenes,
                superposition_cli,
                output_root,
            } => benchmark_scene_survey(scenes, &superposition_cli, &output_root),
        },
        #[cfg(feature = "dev-tools")]
        Command::GenerateAppIcons => {
            exo_dev::brand::icons::generate(&repo_root)?;
            Ok(ExitCode::SUCCESS)
        }
        #[cfg(feature = "dev-tools")]
        Command::GenerateBrandMarks { check } => generate_brand_marks(&repo_root, check),
    }
}

fn benchmark_compare(
    run_dirs: Vec<PathBuf>,
    report_path: Option<PathBuf>,
) -> anyhow::Result<ExitCode> {
    let report = exo_dev::benchmark::compare::compare(&run_dirs)?;
    print!("{}", exo_dev::benchmark::compare::render_table(&report));
    let report_path = report_path.unwrap_or_else(|| {
        run_dirs
            .first()
            .and_then(|dir| dir.parent())
            .unwrap_or(std::path::Path::new("."))
            .join("comparison.json")
    });
    exo_dev::benchmark::compare::write_json(&report, &report_path)?;
    println!();
    println!("comparison dataset: {}", report_path.display());
    Ok(ExitCode::SUCCESS)
}

fn benchmark_scene_survey(
    scenes: Vec<u32>,
    superposition_cli: &std::path::Path,
    output_root: &std::path::Path,
) -> anyhow::Result<ExitCode> {
    let candidates: Vec<PathBuf> = scenes
        .iter()
        .map(|scene| output_root.join(format!("scene-{scene}")))
        .collect();
    let rankings = exo_dev::benchmark::scene_survey::survey(&candidates, superposition_cli)?;
    for ranking in &rankings {
        println!(
            "{:<20} frames={:<6} mean={:.1} median={:.1} p1={:.1} variability={:.2} gpu_util={:?} gpu_temp={:?}",
            ranking.label,
            ranking.stats.frames,
            ranking.stats.mean_fps,
            ranking.stats.median_fps,
            ranking.stats.p1_fps,
            ranking.stats.fps_variability,
            ranking.stats.gpu_util_mean,
            ranking.stats.gpu_temp_max,
        );
    }
    println!();
    println!(
        "Pick a scene on motion and detail, not on score. Then freeze it in the scenario definition."
    );
    Ok(ExitCode::SUCCESS)
}

fn benchmark_topology(repo_root: &std::path::Path, scenario: &str) -> anyhow::Result<ExitCode> {
    let path = exo_dev::benchmark::run::scenario_path(repo_root, scenario);
    let definition = exo_dev::benchmark::run::load_scenario_definition(&path)?;
    let report = exo_dev::benchmark::topology::verify(&definition.topology)?;
    for problem in &report.problems {
        eprintln!("  {problem}");
    }
    println!("attached panels: {}", report.attached_panels.join(", "));
    if let Some(capture) = &report.capture_display {
        println!(
            "capture display: {} {}x{}@{}Hz (primary)",
            capture.device_name, capture.width, capture.height, capture.refresh_hz
        );
    }
    if let Some(ui) = &report.ui_display {
        println!(
            "ui display:      {} {}x{}@{}Hz",
            ui.device_name, ui.width, ui.height, ui.refresh_hz
        );
    }
    if report.ok {
        println!("benchmark topology: OK");
        Ok(ExitCode::SUCCESS)
    } else {
        println!("benchmark topology: FAILED");
        Ok(ExitCode::FAILURE)
    }
}

#[allow(clippy::too_many_arguments)]
fn benchmark_run(
    repo_root: &std::path::Path,
    frontend: &str,
    scenario: &str,
    run_index: u32,
    output_root: Option<PathBuf>,
    widgets_exe: Option<PathBuf>,
    quick_exe: Option<PathBuf>,
    superposition_cli: Option<PathBuf>,
    calibration: bool,
    skip_topology_check: bool,
) -> anyhow::Result<ExitCode> {
    let frontend: Frontend = frontend
        .parse()
        .map_err(|message: String| anyhow::anyhow!(message))?;
    let path = exo_dev::benchmark::run::scenario_path(repo_root, scenario);
    let definition = exo_dev::benchmark::run::load_scenario_definition(&path)?;
    let scenario = exo_dev::benchmark::run::Scenario {
        definition,
        frontend,
        run_index,
        output_root: output_root.unwrap_or_else(|| repo_root.join(".workspace/benchmark-results")),
        widgets_exe,
        quick_exe: quick_exe.unwrap_or_else(|| {
            repo_root.join("build/windows-x64-release-bench/app/Release/exosnap.exe")
        }),
        superposition_cli: superposition_cli.unwrap_or_else(|| {
            PathBuf::from(
                "C:/Program Files/Unigine/Superposition Benchmark/bin/superposition_cli.exe",
            )
        }),
        calibration,
        skip_topology_check,
    };
    let manifest = exo_dev::benchmark::run::run(&scenario)?;
    println!("run complete: {}", manifest.run_id);
    Ok(ExitCode::SUCCESS)
}

#[allow(clippy::too_many_arguments)]
fn benchmark_campaign(
    repo_root: &std::path::Path,
    scenario: &str,
    order: Vec<String>,
    cooldown_seconds: u64,
    output_root: Option<PathBuf>,
    widgets_exe: Option<PathBuf>,
    quick_exe: Option<PathBuf>,
    superposition_cli: Option<PathBuf>,
    calibration: bool,
) -> anyhow::Result<ExitCode> {
    let order: Vec<Frontend> = if order.is_empty() {
        exo_dev::benchmark::campaign::DEFAULT_ORDER.to_vec()
    } else {
        order
            .iter()
            .map(|name| name.parse::<Frontend>())
            .collect::<Result<Vec<_>, String>>()
            .map_err(|message| anyhow::anyhow!(message))?
    };
    let request = exo_dev::benchmark::campaign::CampaignRequest {
        scenario: scenario.to_string(),
        order,
        cooldown: std::time::Duration::from_secs(cooldown_seconds),
        output_root: output_root.unwrap_or_else(|| repo_root.join(".workspace/benchmark-results")),
        widgets_exe,
        quick_exe: quick_exe.unwrap_or_else(|| {
            repo_root.join("build/windows-x64-release-bench/app/Release/exosnap.exe")
        }),
        superposition_cli: superposition_cli.unwrap_or_else(|| {
            PathBuf::from(
                "C:/Program Files/Unigine/Superposition Benchmark/bin/superposition_cli.exe",
            )
        }),
        calibration,
    };
    let result = exo_dev::benchmark::campaign::run_campaign_with(&request)?;
    println!("campaign complete: {} run(s)", result.runs.len());
    Ok(ExitCode::SUCCESS)
}

fn generate_brand_marks(repo_root: &std::path::Path, check: bool) -> anyhow::Result<ExitCode> {
    let report = exo_dev::brand::marks::generate(repo_root, check)?;
    if report.ok() {
        println!("{} marks match parameters.json", report.total);
        return Ok(ExitCode::SUCCESS);
    }
    eprintln!("stale: {}", report.stale.join(", "));
    eprintln!("run: cargo exo-dev generate-brand-marks");
    Ok(ExitCode::FAILURE)
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

fn perf_analyze(args: PerfAnalyzeArgs) -> anyhow::Result<ExitCode> {
    use exo_dev::perf_analyze::{analyze_file, print_delta, print_report, render_json};

    let primary = analyze_file(&args.engine_jsonl)?;
    let secondary = match &args.compare_jsonl {
        Some(path) => Some(analyze_file(path)?),
        None => None,
    };

    if args.json {
        let compare = match (&args.compare_jsonl, &secondary) {
            (Some(path), Some(sessions)) => Some((path.as_path(), sessions.as_slice())),
            _ => None,
        };
        println!("{}", render_json(&args.engine_jsonl, &primary, compare)?);
        return Ok(ExitCode::SUCCESS);
    }

    println!("Perf report: {}", args.engine_jsonl.display());
    print!("{}", print_report(&primary));
    if let Some(secondary) = &secondary {
        println!();
        println!(
            "Perf report: {}",
            args.compare_jsonl
                .as_ref()
                .expect("compare_jsonl set when secondary is Some")
                .display()
        );
        print!("{}", print_report(secondary));
        println!();
        print!("{}", print_delta(&primary, secondary));
    }
    Ok(ExitCode::SUCCESS)
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

#[cfg(feature = "dev-tools")]
fn gen_manifest_fixture() -> anyhow::Result<ExitCode> {
    let fixture = exo_dev::gen_manifest_fixture::generate();
    print!("{}", exo_dev::gen_manifest_fixture::render(&fixture));
    Ok(ExitCode::SUCCESS)
}

fn tool_on_path(tool: &str) -> bool {
    exo_dev::process::command(tool)
        .arg("-version")
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .is_ok()
}

fn av_sync_check(args: AvSyncArgs) -> anyhow::Result<ExitCode> {
    if let Some(times) = &args.marker_times_seconds {
        if times.len() < 2 || times.iter().any(|&t| t < 0.0) {
            bail!("marker times must contain at least 2 non-negative values");
        }
        if times.windows(2).any(|w| w[1] <= w[0]) {
            bail!("marker times must be strictly increasing");
        }
    }
    let mut expected_markers = args.expected_markers;
    if let Some(times) = &args.marker_times_seconds {
        match expected_markers {
            None => expected_markers = Some(times.len()),
            Some(n) if n != times.len() => {
                bail!("--expected-markers must match --marker-times-seconds");
            }
            _ => {}
        }
    }
    if args.max_pair_skew_ms <= 0.0 || args.schedule_tolerance_seconds < 0.0 {
        bail!("pair skew must be positive and schedule tolerance cannot be negative");
    }

    for tool in ["ffmpeg", "ffprobe"] {
        if !tool_on_path(tool) {
            eprintln!(
                "av-sync-check: '{tool}' not found on PATH. Install a full system ffmpeg \
                 (the app's bundled mux-only FFmpeg lacks the required filters)."
            );
            return Ok(ExitCode::from(3));
        }
    }

    // The budget reaches the analysis because reference qualification is
    // relative to it: a fit precise to +/-2 ms qualifies a 20 ms budget and
    // does not qualify a 3 ms one.
    let budget_ms = args.max_drift_ms_per_hour.unwrap_or(args.max_drift_ms);

    let result = exo_dev::av_sync::measure(
        &args.file,
        args.luma_threshold_frac,
        args.rms_threshold_frac,
        budget_ms,
        expected_markers,
        args.marker_times_seconds.as_deref(),
        args.max_pair_skew_ms / 1000.0,
        args.schedule_tolerance_seconds,
    );

    let segment_finding = exo_dev::av_sync::segment_reliability_finding(&result, args.max_drift_ms);

    if args.json {
        println!("{}", av_sync_result_json(&result, segment_finding));
    } else {
        print_av_sync_result(&result);
    }

    let opts = exo_dev::av_sync::VerdictOptions {
        unqualified_reference: args.unqualified_reference,
        max_drift_ms: args.max_drift_ms,
        max_drift_ms_per_hour: args.max_drift_ms_per_hour,
    };
    let verdict = exo_dev::av_sync::verdict(&result, &opts);
    if !args.json {
        match &verdict {
            exo_dev::av_sync::Verdict::CouldNotMeasure { reasons } if !result.measurable => {
                eprintln!("av-sync-check: could not measure: {}", reasons.join("; "));
                eprintln!(
                    "  flash_events={:?} beep_events={:?}",
                    result.flash_events, result.beep_events
                );
            }
            exo_dev::av_sync::Verdict::CouldNotMeasure { reasons } => {
                eprintln!(
                    "av-sync-check: could not measure: the reference signal does not qualify to carry a drift verdict"
                );
                for reason in reasons {
                    eprintln!("  - {reason}");
                }
                eprintln!(
                    "  Re-run with more markers or a longer span, or pass --unqualified-reference to read the numbers without a verdict."
                );
            }
            exo_dev::av_sync::Verdict::OverBudget { measured, budget } => {
                eprintln!("av-sync-check: DRIFT OVER BUDGET - {measured} > {budget}");
            }
            exo_dev::av_sync::Verdict::Pass { measured } => {
                let budget = match args.max_drift_ms_per_hour {
                    Some(rate) => format!("{rate} ms/hour"),
                    None => format!("{} ms", args.max_drift_ms),
                };
                println!("OK: drift {measured} within budget {budget}");
                if segment_finding == Some(true) {
                    eprintln!(
                        "av-sync-check: RELIABILITY FINDING - opposing segment drifts exceed the total-drift budget and cancel at the endpoint"
                    );
                }
            }
        }
    }

    Ok(ExitCode::from(verdict.exit_code() as u8))
}

fn print_av_sync_result(result: &exo_dev::av_sync::MeasureResult) {
    if !result.measurable {
        return;
    }
    println!("span:            {:.3} s", result.span_s);
    println!(
        "offset_start:    {:+.2} ms   (ADVISORY, emission skew, not gated)",
        result.offset_start_ms
    );
    if result.marker_count == 3 {
        println!(
            "offset_middle:   {:+.2} ms   (ADVISORY)",
            result.offset_middle_ms.unwrap_or(0.0)
        );
    }
    println!(
        "offset_end:      {:+.2} ms   (ADVISORY)",
        result.offset_end_ms
    );
    if result.marker_count == 3 {
        println!(
            "drift start->mid: {:+.2} ms",
            result.drift_start_middle_ms.unwrap_or(0.0)
        );
        println!(
            "drift mid->end:   {:+.2} ms",
            result.drift_middle_end_ms.unwrap_or(0.0)
        );
    }
    println!(
        "drift start->end: {:+.2} ms over span   (endpoint, diagnostic)",
        result.drift_start_end_ms
    );
    println!(
        "drift rate:      {:+.2} ms/hour   (endpoint, diagnostic)",
        result.drift_ms_per_hour
    );
    if let Some(fitted) = result.fitted_drift_ms {
        println!(
            "fitted drift:    {:+.2} ms over span +/-{:.2} ms   (VERDICT)",
            fitted,
            result.fitted_drift_uncertainty_ms.unwrap_or(0.0)
        );
        println!(
            "fitted rate:     {:+.2} ms/hour",
            result.fitted_drift_ms_per_hour.unwrap_or(0.0)
        );
        println!(
            "max residual:    {:.2} ms",
            result.max_residual_ms.unwrap_or(0.0)
        );
    }
    let qualified = result
        .reference
        .as_ref()
        .is_some_and(exo_dev::av_sync::Qualification::qualified);
    println!(
        "reference:       {}",
        if qualified {
            "QUALIFIED"
        } else {
            "NOT QUALIFIED"
        }
    );
    if let Some(reference) = &result.reference {
        for reason in reference.reasons() {
            println!("                 - {reason}");
        }
    }
    println!(
        "events:          flash={} beep={} paired={} recognized={}",
        result.flash_event_count,
        result.beep_event_count,
        result.paired_event_count,
        result.marker_count
    );
    println!("flash PTS:       {:?}", result.recognized_flash_pts);
    println!("beep PTS:        {:?}", result.recognized_beep_pts);
}

fn av_sync_result_json(
    result: &exo_dev::av_sync::MeasureResult,
    segment_finding: Option<bool>,
) -> String {
    let mut value = serde_json::json!({
        "file": result.file,
        "flash_events": result.flash_events,
        "beep_events": result.beep_events,
        "flash_event_count": result.flash_event_count,
        "beep_event_count": result.beep_event_count,
        "paired_event_count": result.paired_event_count,
        "measurable": result.measurable,
    });
    let object = value.as_object_mut().unwrap();
    if let Some(error) = &result.error {
        object.insert("error".into(), serde_json::json!(error));
    }
    if result.measurable {
        object.insert(
            "marker_count".into(),
            serde_json::json!(result.marker_count),
        );
        object.insert(
            "markers".into(),
            serde_json::json!(
                result
                    .markers
                    .iter()
                    .map(|m| serde_json::json!({
                        "label": m.label,
                        "flash_s": m.flash_s,
                        "beep_s": m.beep_s,
                        "offset_ms": m.offset_ms,
                    }))
                    .collect::<Vec<_>>()
            ),
        );
        object.insert(
            "recognized_flash_pts".into(),
            serde_json::json!(result.recognized_flash_pts),
        );
        object.insert(
            "recognized_beep_pts".into(),
            serde_json::json!(result.recognized_beep_pts),
        );
        object.insert(
            "flash_start_s".into(),
            serde_json::json!(result.flash_start_s),
        );
        object.insert("flash_end_s".into(), serde_json::json!(result.flash_end_s));
        object.insert(
            "beep_start_s".into(),
            serde_json::json!(result.beep_start_s),
        );
        object.insert("beep_end_s".into(), serde_json::json!(result.beep_end_s));
        object.insert("span_s".into(), serde_json::json!(result.span_s));
        object.insert(
            "offset_start_ms".into(),
            serde_json::json!(result.offset_start_ms),
        );
        object.insert(
            "offset_end_ms".into(),
            serde_json::json!(result.offset_end_ms),
        );
        object.insert("drift_ms".into(), serde_json::json!(result.drift_ms));
        object.insert(
            "drift_start_end_ms".into(),
            serde_json::json!(result.drift_start_end_ms),
        );
        object.insert(
            "drift_ms_per_hour".into(),
            serde_json::json!(result.drift_ms_per_hour),
        );
        if let Some(v) = result.offset_middle_ms {
            object.insert("offset_middle_ms".into(), serde_json::json!(v));
        }
        if let Some(v) = result.drift_start_middle_ms {
            object.insert("drift_start_middle_ms".into(), serde_json::json!(v));
        }
        if let Some(v) = result.drift_middle_end_ms {
            object.insert("drift_middle_end_ms".into(), serde_json::json!(v));
        }
        if let Some(fit) = &result.fit {
            object.insert(
                "fit".into(),
                serde_json::json!({
                    "fitted": true,
                    "slope_s_per_s": fit.slope_s_per_s,
                    "intercept_s": fit.intercept_s,
                    "slope_standard_error_s_per_s": fit.slope_standard_error_s_per_s,
                    "residuals_s": fit.residuals_s,
                    "max_abs_residual_s": fit.max_abs_residual_s,
                    "marker_uncertainties_s": fit.marker_uncertainties_s,
                }),
            );
        }
        if let Some(reference) = &result.reference {
            let drift_uncertainty_ms = result.fitted_drift_uncertainty_ms.unwrap_or(0.0);
            object.insert(
                "reference".into(),
                serde_json::json!({
                    "qualified": reference.qualified(),
                    "reasons": reference.reasons(),
                    "drift_uncertainty_ms": drift_uncertainty_ms,
                    "allowed_uncertainty_ms": result.budget_ms / 3.0,
                }),
            );
        }
        if let Some(v) = result.fitted_drift_ms {
            object.insert("fitted_drift_ms".into(), serde_json::json!(v));
        }
        if let Some(v) = result.fitted_drift_ms_per_hour {
            object.insert("fitted_drift_ms_per_hour".into(), serde_json::json!(v));
        }
        if let Some(v) = result.fitted_emission_skew_ms {
            object.insert("fitted_emission_skew_ms".into(), serde_json::json!(v));
        }
        if let Some(v) = result.fitted_drift_uncertainty_ms {
            object.insert("fitted_drift_uncertainty_ms".into(), serde_json::json!(v));
        }
        if let Some(v) = result.max_residual_ms {
            object.insert("max_residual_ms".into(), serde_json::json!(v));
        }
        if let Some(finding) = segment_finding {
            object.insert(
                "segment_reliability_finding".into(),
                serde_json::json!(finding),
            );
        }
    }
    serde_json::to_string_pretty(&value).unwrap_or_default()
}

fn gen_av_sync_fixture(repo_root: &std::path::Path, args: FixtureArgs) -> anyhow::Result<ExitCode> {
    if !tool_on_path("ffmpeg") {
        eprintln!("gen-av-sync-fixture: ffmpeg not found on PATH.");
        return Ok(ExitCode::from(3));
    }
    let out = args
        .out
        .unwrap_or_else(|| exo_dev::av_sync::fixture::default_out(repo_root));
    let spec = exo_dev::av_sync::fixture::FixtureSpec {
        duration: args.duration,
        markers: args.markers,
        out: out.clone(),
    };
    let times = match exo_dev::av_sync::fixture::generate(&spec) {
        Ok(times) => times,
        Err(error) => {
            eprintln!("gen-av-sync-fixture: {error:#}");
            return Ok(ExitCode::from(64));
        }
    };
    let size = std::fs::metadata(&out).map(|m| m.len()).unwrap_or(0);
    println!("wrote {} ({size} bytes)", out.display());
    println!(
        "markers at {} s",
        times
            .iter()
            .map(|t| format!("{t:.3}"))
            .collect::<Vec<_>>()
            .join(", ")
    );
    Ok(ExitCode::SUCCESS)
}

fn read_crash_dump(args: CrashDumpArgs) -> anyhow::Result<ExitCode> {
    let report = exo_dev::crash_dump::read(&args.dump, &args.pdb_search_path)?;
    print!("{}", exo_dev::crash_dump::render(&report));
    Ok(ExitCode::SUCCESS)
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

/// Resolves an explicit `--version` or falls back to the canonical CMake
/// project version, every packaging validator's own default.
fn resolve_packaging_version(
    repo_root: &std::path::Path,
    version: Option<String>,
) -> anyhow::Result<String> {
    match version {
        Some(version) => Ok(version),
        None => exo_dev::packaging::cmake_project_version(repo_root),
    }
}

fn check_packaging_version(
    repo_root: &std::path::Path,
    version: Option<String>,
) -> anyhow::Result<ExitCode> {
    let report = exo_dev::release::version::check_packaging_version(repo_root, version.as_deref())?;
    print!("{}", exo_dev::release::version::render_drift(&report));
    Ok(if report.ok() {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

fn release_bump_version(
    repo_root: &std::path::Path,
    version: &str,
    force: bool,
) -> anyhow::Result<ExitCode> {
    let outcome = exo_dev::release::version::bump(repo_root, version, force)?;
    print!("{}", exo_dev::release::version::render_bump(&outcome));
    Ok(if outcome.ok() {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

/// Every packaging validator's exit code convention: 0 clean, 1 for any
/// failure, whether that failure is a specific finding or the target
/// manifest could not even be read. There is no distinct "could not run"
/// exit code.
fn packaging_chocolatey(
    repo_root: &std::path::Path,
    version: Option<String>,
    version_only: bool,
    manifest_path: Option<PathBuf>,
    require_manifest: bool,
) -> anyhow::Result<ExitCode> {
    let version = resolve_packaging_version(repo_root, version)?;
    let mode = exo_dev::packaging::ChocolateyMode {
        version_only,
        manifest_path,
        require_manifest,
    };
    match exo_dev::packaging::validate_chocolatey(repo_root, &version, mode) {
        Ok(report) => {
            print!("{}", exo_dev::packaging::render(&report));
            if report.ok() {
                if version_only {
                    println!(
                        "Chocolatey version validation PASSED for ExoSnap {version} (nuspec + chocolateyinstall.ps1 name one version)."
                    );
                } else {
                    println!(
                        "Chocolatey package validation PASSED for ExoSnap {version} (nuspec + chocolateyinstall.ps1 consistent)."
                    );
                }
                Ok(ExitCode::SUCCESS)
            } else {
                let kind = if version_only { "version" } else { "package" };
                println!(
                    "Chocolatey {kind} validation FAILED ({} error(s)) for version {version}.",
                    report.errors.len()
                );
                Ok(ExitCode::FAILURE)
            }
        }
        Err(error) => {
            eprintln!("{error:#}");
            Ok(ExitCode::FAILURE)
        }
    }
}

fn packaging_winget(
    repo_root: &std::path::Path,
    version: Option<String>,
) -> anyhow::Result<ExitCode> {
    let version = resolve_packaging_version(repo_root, version)?;
    match exo_dev::packaging::validate_winget(repo_root, &version) {
        Ok(report) => {
            print!("{}", exo_dev::packaging::render(&report));
            if report.ok() {
                println!(
                    "WinGet manifest validation PASSED for Codexo.ExoSnap {version} (3 files, dependency on Microsoft.VCRedist.2015+.x64 confirmed)."
                );
                Ok(ExitCode::SUCCESS)
            } else {
                println!(
                    "WinGet manifest validation FAILED ({} error(s)) for version {version}.",
                    report.errors.len()
                );
                Ok(ExitCode::FAILURE)
            }
        }
        Err(error) => {
            eprintln!("{error:#}");
            Ok(ExitCode::FAILURE)
        }
    }
}

fn packaging_scoop(
    repo_root: &std::path::Path,
    version: Option<String>,
) -> anyhow::Result<ExitCode> {
    let version = resolve_packaging_version(repo_root, version)?;
    match exo_dev::packaging::validate_scoop(repo_root, &version) {
        Ok(report) => {
            print!("{}", exo_dev::packaging::render(&report));
            if report.ok() {
                println!("Scoop manifest validation PASSED for ExoSnap {version}.");
                Ok(ExitCode::SUCCESS)
            } else {
                println!(
                    "Scoop manifest validation FAILED ({} error(s)) for version {version}.",
                    report.errors.len()
                );
                Ok(ExitCode::FAILURE)
            }
        }
        Err(error) => {
            eprintln!("{error:#}");
            Ok(ExitCode::FAILURE)
        }
    }
}

fn packaging_msi_harvest(repo_root: &std::path::Path) -> anyhow::Result<ExitCode> {
    match exo_dev::packaging::validate_msi_harvest(repo_root) {
        Ok(report) => {
            print!("{}", exo_dev::packaging::render(&report));
            if report.ok() {
                println!(
                    "MSI harvest validation PASSED (Package.wxs is metadata-only, references StagingFiles)."
                );
                Ok(ExitCode::SUCCESS)
            } else {
                println!(
                    "MSI harvest validation FAILED ({} error(s)).",
                    report.errors.len()
                );
                Ok(ExitCode::FAILURE)
            }
        }
        Err(error) => {
            eprintln!("{error:#}");
            Ok(ExitCode::FAILURE)
        }
    }
}

fn release_changelog(
    repo_root: &std::path::Path,
    since: Option<String>,
    until: Option<String>,
    version: Option<String>,
    date: Option<String>,
    apply: bool,
) -> anyhow::Result<ExitCode> {
    use exo_dev::release::changelog;

    if let Some(version) = &version {
        static VERSION_PATTERN: std::sync::LazyLock<regex::Regex> =
            std::sync::LazyLock::new(|| regex::Regex::new(r"^[0-9]+\.[0-9]+\.[0-9]+$").unwrap());
        anyhow::ensure!(
            VERSION_PATTERN.is_match(version),
            "Version '{version}' is not x.y.z."
        );
    }

    let assembly = changelog::assemble(
        repo_root,
        since.as_deref(),
        until.as_deref(),
        &exo_dev::commit_policy::CommitPolicyOptions::default(),
    )?;
    let rendered = changelog::render(&assembly, version.as_deref(), date.as_deref());
    print!("{}", changelog::render_report(&assembly));

    if apply {
        let path = changelog::apply(repo_root, &rendered, version.is_some())?;
        println!("\nwrote     {}", path.display());
    } else {
        println!();
        for line in &rendered {
            println!("{line}");
        }
    }

    if assembly.unreadable.is_empty() {
        Ok(ExitCode::SUCCESS)
    } else {
        Ok(ExitCode::FAILURE)
    }
}

#[allow(clippy::too_many_arguments)]
fn release_release_notes(
    repo_root: &std::path::Path,
    version: String,
    tag: Option<String>,
    previous_tag: Option<String>,
    commit: Option<String>,
    candidate: bool,
    repository_url: Option<String>,
    out_file: Option<PathBuf>,
) -> anyhow::Result<ExitCode> {
    use exo_dev::release::changelog;

    let repository_url =
        repository_url.unwrap_or_else(|| changelog::DEFAULT_REPOSITORY_URL.to_string());
    let request = changelog::ReleaseNotesRequest {
        version: &version,
        tag: tag.as_deref(),
        previous_tag: previous_tag.as_deref(),
        commit: commit.as_deref(),
        candidate,
        repository_url: &repository_url,
    };
    let rendered = changelog::render_notes(repo_root, &request)?;
    if rendered.warn_no_previous_tag {
        eprintln!(
            "warning: No previous version tag was found; the compare link has an empty left side."
        );
    }

    match out_file {
        Some(path) => {
            std::fs::write(&path, &rendered.text)?;
            println!("wrote {}", path.display());
        }
        None => print!("{}", rendered.text),
    }
    Ok(ExitCode::SUCCESS)
}

fn release_feed_drift(
    repo_root: &std::path::Path,
    offline: bool,
    policy_path: Option<PathBuf>,
    timeout_seconds: u64,
) -> anyhow::Result<ExitCode> {
    use exo_dev::release::feed_drift;

    let policy_path =
        policy_path.unwrap_or_else(|| repo_root.join("packaging/publication-policy.json"));
    let report = feed_drift::check(repo_root, &policy_path, !offline, timeout_seconds)?;
    print!("{}", feed_drift::render(&report));
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
