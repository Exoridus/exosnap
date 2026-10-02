//! `exo-dev test`: build one CMake tree, run its CTest suite in an isolated
//! environment, and publish a receipt saying what the run established.
//!
//! What makes the result evidence: build, suite and receipt happen under one
//! host lock on the build directory, so no other cooperating entry point
//! configures or rebuilds the tree while this run decides what it holds. The
//! source identity is taken under that lock before the build and again before
//! the receipt, so a change made to the working tree mid-run is reported as
//! drift rather than attributed to the result. Every run that got the tree
//! publishes a receipt, including a failing one: a broken run must never leave
//! an older successful receipt standing as its outcome. `reusable` is the one
//! field a consumer reads.
//!
//! Locks, in the host-wide order tree, build, device:
//! - tree: the whole span from the build to the published receipt.
//! - build: around `cmake --build` only.
//! - device: around the `ctest` run only.
//!
//! Children run under those holds: each lock's inherit mark is set on the
//! child's command while the lock is held, so a test that starts another run
//! on the same tree or device runs under this one instead of waiting on it.
//!
//! Exit codes:
//! - 0: the selected tests passed and the run is valid.
//! - 2: the build directory does not exist.
//! - 3: the tree was not proven to match the source (see `allow_stale`).
//! - 4: the run is not a valid verification: a census, phase or
//!   source-identity contradiction, or evidence that could not be secured.
//! - Anything else: the build's or ctest's own exit code.

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

use anyhow::Context as _;

use super::catalog::{self, Phase};
use super::environment::{self, ChildEnv, QtInstall};
use super::fingerprint::{self, SourceIdentity};
use super::freshness;
use super::host::{Checkpoint, Console, HeldLock, Host};
use super::receipt::{self, EXIT_INVALID_RUN, Freshness, Receipt};
use super::rescue::{self, Rescue};
use super::summary;
use crate::host_lock::{self, LockKind};

/// Names the executable running the suite, for a test that has to run
/// exo-dev itself. Such a test runs this binary, never `cargo run`, which
/// would try to relink an executable Windows keeps locked while it runs. Set
/// for the ctest child only.
pub const TOOL_EXE_VARIABLE: &str = "EXOSNAP_TEST_TOOL_EXE";

/// What to run. Mirrors the `exo-dev test` flags.
#[derive(Clone, Debug)]
pub struct Options {
    /// The repository whose source the result is about.
    pub repo_root: PathBuf,
    /// Relative paths are taken from `repo_root`.
    pub build_dir: PathBuf,
    pub config: String,
    /// `ctest -R`.
    pub filter: String,
    /// A plain label word is anchored, see [`catalog::exclude_pattern`].
    pub exclude_label: String,
    pub phase: Option<Phase>,
    /// Zero means the host job budget.
    pub jobs: usize,
    pub no_build: bool,
    pub allow_stale: bool,
    /// Applied to every child before anything this run sets, e.g. an MSVC
    /// environment the caller already imported.
    pub child_env: Vec<(std::ffi::OsString, std::ffi::OsString)>,
}

impl Options {
    pub fn new(repo_root: &Path) -> Options {
        Options {
            repo_root: repo_root.to_path_buf(),
            build_dir: PathBuf::from("build/windows-x64-ninja-debug"),
            config: "Debug".into(),
            filter: String::new(),
            exclude_label: String::new(),
            phase: None,
            jobs: 0,
            no_build: false,
            allow_stale: false,
            child_env: Vec::new(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct RunResult {
    pub exit_code: i32,
    /// The tests ctest reported as failing, for a run whose suite failed.
    pub failed_tests: Vec<String>,
    /// The full ctest output.
    pub log: PathBuf,
    pub receipt_path: PathBuf,
    /// This run's receipt, for a run that got the tree lock. A run that did
    /// not writes none: the file belongs to whoever holds the tree.
    pub receipt: Option<Receipt>,
    pub receipt_published: bool,
}

const SEPARATOR: &str = "------------------------------------------------------------";

/// Builds, runs and judges one tree. Never returns an error: every failure is
/// an exit code, a stated reason and, once the tree lock was held, a receipt.
pub fn run(options: &Options, host: &dyn Host, console: &mut Console) -> RunResult {
    let build_dir = if options.build_dir.is_absolute() {
        options.build_dir.clone()
    } else {
        options.repo_root.join(&options.build_dir)
    };
    let log_dir = build_dir.join("Testing");
    let log = log_dir.join("last-run.log");
    let receipt_path = log_dir.join("last-run-receipt.json");

    if !build_dir.is_dir() {
        console.line(format!(
            "Build dir '{}' does not exist. Configure it first (cmake --preset ...) or pass --build-dir.",
            build_dir.display()
        ));
        return RunResult {
            exit_code: 2,
            failed_tests: Vec::new(),
            log,
            receipt_path,
            receipt: None,
            receipt_published: false,
        };
    }

    // Bounded by the same budget the verify runner uses, never the full core
    // count: a test run at -j<cores> beside a build in another worktree is the
    // contention the host locks exist for.
    let jobs = if options.jobs > 0 {
        options.jobs
    } else {
        host_lock::job_budget(std::env::var("EXOSNAP_VERIFY_JOBS").ok().as_deref())
    };
    let run_id = receipt::new_run_id();
    let receipt = Receipt {
        run_id: run_id.clone(),
        started_utc: receipt::utc_now(),
        finished_utc: None,
        build_dir: build_dir.display().to_string(),
        generator: String::new(),
        config: options.config.clone(),
        jobs,
        freshness: Freshness::Unknown,
        freshness_detail: "the run did not get far enough to judge the tree".into(),
        allow_stale: options.allow_stale,
        no_build: options.no_build,
        build_status: if options.no_build {
            "skipped"
        } else {
            "not-started"
        },
        build_exit_code: None,
        ctest_args: Vec::new(),
        exclude_label: options.exclude_label.clone(),
        exclude_pattern: String::new(),
        phase: options.phase.map(Phase::name).unwrap_or_default().into(),
        filter: options.filter.clone(),
        tests_registered: 0,
        tests_disabled: 0,
        tests_selected: 0,
        tests_accounted: 0,
        tests_expected: 0,
        tests_passed: 0,
        tests_failed: 0,
        census_mismatch: false,
        phase_violations: None,
        ctest_exit_code: None,
        source_before: None,
        source_after: None,
        source_drift: None,
        log: log.display().to_string(),
        rescued_config_dir: None,
        rescue_status: "not-needed",
        rescue_detail: None,
        invalid_reasons: Vec::new(),
        exit_code: EXIT_INVALID_RUN,
        reusable: false,
    };

    let mut state = Run {
        options,
        host,
        console,
        build_dir,
        log_dir,
        log,
        receipt_path,
        run_id,
        jobs,
        qt: QtInstall::default(),
        msvc: Vec::new(),
        config_dir: None,
        keep_config_dir: false,
        tree_mark: None,
        receipt,
        completed: false,
        published: false,
        failed_tests: Vec::new(),
    };
    let exit_code = state.start();
    state.remove_config_dir();

    RunResult {
        exit_code,
        failed_tests: state.failed_tests,
        log: state.log,
        receipt_path: state.receipt_path,
        receipt: state.completed.then_some(state.receipt),
        receipt_published: state.published,
    }
}

struct Run<'a> {
    options: &'a Options,
    host: &'a dyn Host,
    console: &'a mut Console,
    build_dir: PathBuf,
    log_dir: PathBuf,
    log: PathBuf,
    receipt_path: PathBuf,
    run_id: String,
    jobs: usize,
    qt: QtInstall,
    msvc: Vec<(String, String)>,
    config_dir: Option<PathBuf>,
    /// Set when the evidence in the configuration directory could not be
    /// secured: the original is then the only copy and must survive.
    keep_config_dir: bool,
    tree_mark: Option<(String, String)>,
    receipt: Receipt,
    completed: bool,
    published: bool,
    failed_tests: Vec<String>,
}

impl Run<'_> {
    fn start(&mut self) -> i32 {
        // Resolved from .qt-version, never spelled out: a hard-coded path keeps
        // working after a Qt uplift, against the previous Qt.
        let qt = environment::resolve_qt(
            &self.options.repo_root,
            self.host.var("EXOSNAP_QT_ROOT").as_deref(),
            self.host.var("SystemDrive").as_deref(),
        );
        self.qt = match qt {
            Ok(qt) => qt,
            Err(error) => return self.did_not_start(&error),
        };
        if let Some(warning) = self.qt.warning.clone() {
            self.console.line(warning);
        }

        // Never the user's real configuration.
        let config_dir = std::env::temp_dir().join(format!("exosnap_runtests_{}", self.run_id));
        if let Err(error) = std::fs::create_dir_all(&config_dir) {
            let error = anyhow::Error::new(error).context(format!(
                "could not create the throwaway config dir {}",
                config_dir.display()
            ));
            return self.did_not_start(&error);
        }
        self.config_dir = Some(config_dir);

        // Everything that reads or writes this build tree happens inside this
        // hold, including the closing fingerprint and the receipt: released
        // earlier, a second run could take the tree while this one is still
        // deciding what its result describes.
        let holder = format!("exo-dev test {}", std::process::id());
        let tree = match self
            .host
            .acquire(LockKind::Tree, Some(&self.build_dir), &holder)
        {
            Ok(tree) => tree,
            Err(error) => return self.did_not_start(&error),
        };
        if tree.waited() >= Duration::from_secs(2) {
            self.console.line(format!(
                "  waited {:.0}s for the tree lock on {} held by another run",
                tree.waited().as_secs_f64(),
                self.build_dir.display()
            ));
        }
        self.tree_mark = Some(tree.child_env());

        let exit_code = match self.invoke() {
            Ok(code) => code,
            Err(error) => {
                self.console
                    .line(format!("The test run did not complete: {error:#}"));
                self.invalid(format!("run aborted: {error:#}"));
                EXIT_INVALID_RUN
            }
        };
        let exit_code = self.complete(exit_code);
        drop(tree);
        exit_code
    }

    /// The run never held the tree. No receipt: the file describes the tree,
    /// the tree belongs to another run, and publishing would replace its
    /// verdict with this run's inability to start.
    fn did_not_start(&mut self, error: &anyhow::Error) -> i32 {
        self.console
            .line(format!("The test run did not start: {error:#}"));
        if self.receipt_path.is_file() {
            self.console.line(format!(
                "No receipt was written: {} belongs to the run that holds this tree.",
                self.receipt_path.display()
            ));
        }
        EXIT_INVALID_RUN
    }

    fn invalid(&mut self, reason: impl Into<String>) {
        self.receipt.invalid_reasons.push(reason.into());
    }

    /// Build, run and judge, with the tree held. An early return is an exit
    /// code the caller reports after the receipt is published.
    fn invoke(&mut self) -> anyhow::Result<i32> {
        std::fs::create_dir_all(&self.log_dir)
            .with_context(|| format!("could not create {}", self.log_dir.display()))?;

        // An earlier run's receipt is not this run's result. Removed before the
        // work starts, so a crash before the publish leaves no verdict rather
        // than the previous one.
        if self.receipt_path.exists() {
            let _ = std::fs::remove_file(&self.receipt_path);
        }

        let before = fingerprint::identify(&self.options.repo_root);
        self.receipt.source_before = Some(before.clone());
        if let SourceIdentity::Unavailable { reason } = &before {
            self.console.line(format!(
                "Cannot identify the source this run is about: {reason}"
            ));
            self.invalid(format!("source identity unavailable: {reason}"));
            return Ok(EXIT_INVALID_RUN);
        }

        let cache = self.build_dir.join("CMakeCache.txt");
        let generator = freshness::cache_value(&cache, "CMAKE_GENERATOR").unwrap_or_default();
        self.receipt.generator = generator.clone();
        let ninja = generator.to_ascii_lowercase().contains("ninja");

        // Build by default, so the run is its own evidence: a build that fails
        // leaves the previous binaries in place, and a suite run against those
        // passes and reads exactly like a pass for the change.
        if !self.options.no_build {
            if ninja {
                self.enter_msvc()?;
            }
            self.console.line(format!(
                "Building all targets in {} ({})...",
                self.build_dir.display(),
                self.options.config
            ));
            let code = self.build()?;
            self.receipt.build_exit_code = Some(code);
            if code != 0 {
                self.receipt.build_status = "failed";
                self.console.line(format!("Build failed (exit {code})."));
                return Ok(code);
            }
            self.receipt.build_status = "succeeded";
        }
        std::fs::create_dir_all(&self.log_dir)
            .with_context(|| format!("could not create {}", self.log_dir.display()))?;

        let (freshness, detail) = self.freshness(ninja, &cache)?;
        self.receipt.freshness = freshness;
        self.receipt.freshness_detail = detail.clone();
        let build_dir = self.build_dir.display().to_string();
        match freshness {
            Freshness::Fresh => {}
            Freshness::Stale => {
                self.console.line(format!("Build tree is STALE: {detail}"));
                self.console.line(format!("  {build_dir}"));
                if !self.options.allow_stale {
                    self.console.line(
                        "Refusing to report a result for binaries that are not the source in front \
                         of us. Re-run without --no-build.",
                    );
                    self.invalid(format!("build tree is stale: {detail}"));
                    return Ok(3);
                }
                self.console.line(
                    "Continuing anyway (--allow-stale was passed). The result describes the OLD binaries.",
                );
            }
            Freshness::Unknown => {
                self.console.line(format!(
                    "Cannot prove this build tree matches the source: {detail}"
                ));
                self.console.line(format!("  {build_dir}"));
                if !self.options.allow_stale {
                    self.invalid(format!("build tree freshness unknown: {detail}"));
                    return Ok(3);
                }
                // Said on the way past, not only at the refusal: an unproven
                // tree that is waved through prints a normal summary otherwise.
                self.console.line(
                    "Continuing anyway (--allow-stale was passed). The result may describe OLD binaries.",
                );
            }
        }

        let pattern = catalog::exclude_pattern(&self.options.exclude_label);
        self.receipt.exclude_pattern = pattern.clone();
        let mut selection: Vec<String> = Vec::new();
        if !self.options.filter.is_empty() {
            selection.extend(["-R".into(), self.options.filter.clone()]);
        }
        if !pattern.is_empty() {
            selection.extend(["-LE".into(), pattern]);
        }
        if let Some(phase) = self.options.phase {
            selection.extend(["-L".into(), format!("^phase\\.{}$", phase.name())]);
        }
        let mut ctest_args: Vec<String> = vec![
            "--test-dir".into(),
            build_dir.clone(),
            "-C".into(),
            self.options.config.clone(),
            "-j".into(),
            self.jobs.to_string(),
            "--output-on-failure".into(),
            // A selection that matches nothing is a configuration mistake,
            // never a pass.
            "--no-tests=error".into(),
        ];
        ctest_args.extend(selection.iter().cloned());
        self.receipt.ctest_args = ctest_args.clone();

        // The census: what the tree registers at all, independent of this
        // run's selection. A suite that lost half its cases to a missing tool
        // at configure time still passes everything it kept.
        let env = self.env_for(&[]).0;
        let registered = match catalog::query(&self.build_dir, &self.options.config, &[], &env) {
            Ok(tests) => tests,
            Err(reason) => {
                self.console.line(format!(
                    "Cannot read the test catalog of {build_dir} : {reason}"
                ));
                self.invalid(format!("test catalog unavailable: {reason}"));
                return Ok(EXIT_INVALID_RUN);
            }
        };
        self.receipt.tests_registered = registered.len();
        self.receipt.tests_disabled = registered.iter().filter(|t| t.disabled).count();
        if registered.is_empty() {
            self.console.line(format!(
                "The build tree registers no tests at all: {build_dir}"
            ));
            self.invalid("the build tree registers no tests");
            return Ok(EXIT_INVALID_RUN);
        }

        let phases = catalog::check_phases(&registered);
        self.receipt.phase_violations = Some(phases.clone());
        if !phases.ok() {
            self.console.line("");
            self.console.line(
                "FAIL  the execution phases this tree declares do not let a phase selection mean anything:",
            );
            self.name_list(
                &format!("no phase ({}):", phases.missing.len()),
                &phases.missing,
            );
            self.name_list(
                &format!("more than one phase ({}):", phases.multiple.len()),
                &phases.multiple,
            );
            self.name_list(
                &format!("phase outside the vocabulary ({}):", phases.unknown.len()),
                &phases.unknown,
            );
            self.console.line(
                "      Give each one exactly one PHASE in exosnap_add_gtest, or an \
                 exosnap_set_test_phase(...) beside its add_test.",
            );
            self.invalid(format!(
                "phase declarations invalid: {} missing, {} duplicated, {} unknown",
                phases.missing.len(),
                phases.multiple.len(),
                phases.unknown.len()
            ));
            return Ok(EXIT_INVALID_RUN);
        }

        let selected = match catalog::query(&self.build_dir, &self.options.config, &selection, &env)
        {
            Ok(tests) => tests,
            Err(reason) => {
                self.console.line(format!(
                    "Cannot read the selected tests of {build_dir} : {reason}"
                ));
                self.invalid(format!("test selection unavailable: {reason}"));
                return Ok(EXIT_INVALID_RUN);
            }
        };
        self.receipt.tests_selected = selected.len();
        // ctest leaves a disabled test out of the summary it prints, so what
        // has to be accounted for is the selection without them.
        let expected = selected.iter().filter(|t| !t.disabled).count();
        self.receipt.tests_expected = expected;
        if expected == 0 {
            self.console.line(format!(
                "The selection matches no test that would run (of {} selected).",
                selected.len()
            ));
            self.invalid("the selection matches no test that would run");
            return Ok(EXIT_INVALID_RUN);
        }

        self.console.line(format!("ctest {}", ctest_args.join(" ")));
        self.console.line(format!(
            "Build tree: {build_dir} [{generator}] freshness={}",
            freshness.as_str()
        ));
        self.console.line(format!(
            "Selected {} of {} registered tests",
            selected.len(),
            registered.len()
        ));
        self.console
            .line(format!("Full log: {}", self.log.display()));
        self.console.line("");

        // The rescue runs whatever the suite call returned, including an
        // error with the suite already run and its logs already written: that
        // is the run whose logs are worth the most. No verdict counts as "not
        // evaluated", never as a pass.
        let suite = self.suite(&ctest_args);
        self.save_evidence(suite.as_ref().ok().copied());
        let ctest_exit = suite?;
        self.receipt.ctest_exit_code = Some(ctest_exit);

        self.judge(ctest_exit, expected)
    }

    /// Makes cl.exe reachable for a Ninja tree. A no-op when it resolves
    /// already, so a caller's own developer environment is never replaced.
    fn enter_msvc(&mut self) -> anyhow::Result<()> {
        let path = ChildEnv(self.options.child_env.clone())
            .get("PATH")
            .unwrap_or_default();
        if crate::msvc::find_compiler(&path.to_string_lossy()).is_some() {
            return Ok(());
        }
        if let Some(variables) = self.host.msvc_environment()? {
            self.msvc = variables;
        }
        Ok(())
    }

    fn acquire(&mut self, kind: LockKind) -> anyhow::Result<Box<dyn HeldLock>> {
        let holder = format!("exo-dev test {}", self.build_dir.display());
        let lock = self.host.acquire(kind, None, &holder)?;
        if lock.waited() >= Duration::from_secs(2) {
            let name = format!("{kind:?}").to_lowercase();
            self.console.line(format!(
                "  waited {:.0}s for the host {name} lock held by another run",
                lock.waited().as_secs_f64()
            ));
        }
        Ok(lock)
    }

    fn build(&mut self) -> anyhow::Result<i32> {
        let lock = self.acquire(LockKind::Build)?;
        self.host.checkpoint(Checkpoint::BeforeBuild)?;
        let mut command = crate::process::command("cmake");
        command
            .arg("--build")
            .arg(&self.build_dir)
            .args(["--config", &self.options.config]);
        self.env_for(&[lock.child_env()]).apply(&mut command);
        let code = self
            .stream(command)
            .context("cmake --build could not be run")?;
        self.host.checkpoint(Checkpoint::AfterBuild)?;
        Ok(code)
    }

    /// Runs `command` with its combined output forwarded to the console.
    fn stream(&mut self, mut command: Command) -> std::io::Result<i32> {
        let (reader, writer) = std::io::pipe()?;
        command
            .stdin(Stdio::null())
            .stdout(writer.try_clone()?)
            .stderr(writer);
        let mut child = command.spawn()?;
        // Closes this process's copies of the write end, or the read below
        // never sees end of file.
        drop(command);
        let mut lines = BufReader::new(reader);
        let mut line = Vec::new();
        loop {
            line.clear();
            if lines.read_until(b'\n', &mut line)? == 0 {
                break;
            }
            let text = String::from_utf8_lossy(&line);
            self.console.line(text.trim_end_matches(['\r', '\n']));
        }
        Ok(child.wait()?.code().unwrap_or(-1))
    }

    /// Whether the tree in front of us is the source in front of us. A build
    /// by this run settles it. Without one, only a Ninja tree can even be
    /// asked, and a timestamp comparison is not an answer: a copied or
    /// restored tree keeps the mtimes it was created with.
    fn freshness(&mut self, ninja: bool, cache: &Path) -> anyhow::Result<(Freshness, String)> {
        if !self.options.no_build {
            return Ok((Freshness::Fresh, "built by this invocation".into()));
        }
        let unknown = |detail: &str| Ok((Freshness::Unknown, detail.to_string()));
        if !ninja {
            return unknown(
                "this generator cannot report staleness without building; drop --no-build",
            );
        }
        let program = freshness::cache_value(cache, "CMAKE_MAKE_PROGRAM").unwrap_or_default();
        if program.is_empty() || !Path::new(&program).exists() {
            return unknown(
                "this generator cannot report staleness without building; drop --no-build",
            );
        }
        let mut command = crate::process::command(&program);
        command.arg("-C").arg(&self.build_dir).arg("-n");
        self.env_for(&[]).apply(&mut command);
        let output = command
            .stdin(Stdio::null())
            .output()
            .with_context(|| format!("{program} could not be run"))?;
        if !output.status.success() {
            return unknown("ninja could not evaluate the graph");
        }
        let text = format!(
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        Ok(freshness::dry_run_verdict(&text))
    }

    /// Runs the selection under the host device lock. RESOURCE_LOCK and
    /// RUN_SERIAL serialise tests within one ctest. A second ctest in another
    /// worktree, a live check or a VM campaign shares the GPU and the desktop
    /// with it and none of them can see the others.
    fn suite(&mut self, ctest_args: &[String]) -> anyhow::Result<i32> {
        let lock = self.acquire(LockKind::Device)?;
        self.host.checkpoint(Checkpoint::BeforeSuite)?;
        let log = File::create(&self.log).with_context(|| {
            format!(
                "could not record the suite output to {}",
                self.log.display()
            )
        })?;
        let mut command = crate::process::command("ctest");
        command.args(ctest_args);
        let mut env = self.env_for(&[lock.child_env()]);
        match std::env::current_exe() {
            Ok(exe) => env.set(TOOL_EXE_VARIABLE, exe),
            Err(error) => self.console.line(format!(
                "{TOOL_EXE_VARIABLE} is not set for the suite: this executable's path is unknown ({error})"
            )),
        }
        env.apply(&mut command);
        // A file, not a pipe: a process a test leaves running cannot hold the
        // run open by keeping an inherited pipe handle.
        let status = command
            .stdin(Stdio::null())
            .stdout(log.try_clone()?)
            .stderr(log)
            .status()
            .context("ctest could not be run")?;
        let code = status.code().unwrap_or(-1);
        self.host.checkpoint(Checkpoint::AfterSuiteBeforeVerdict)?;
        Ok(code)
    }

    /// The verdict over the suite's log: counts, the census, and what failed.
    fn judge(&mut self, ctest_exit: i32, expected: usize) -> anyhow::Result<i32> {
        let lines = crate::process::read_lines(&self.log);
        let counts = summary::summary(&lines);
        self.receipt.tests_passed = counts.passed;
        self.receipt.tests_failed = counts.failed;
        self.receipt.tests_accounted = counts.passed + counts.failed;

        // Selected but never accounted for: ctest dropped tests between the
        // listing and the run. Reporting only the ones that ran would turn
        // that into a pass, so the contradiction is the verdict.
        let accounted = counts.passed + counts.failed;
        let census_mismatch = !counts.parsed || accounted != expected;
        self.receipt.census_mismatch = census_mismatch;

        self.console.line(SEPARATOR);
        if let Some(line) = &counts.line {
            self.console.line(line.trim());
        }
        if let Some(line) = &counts.time_line {
            self.console.line(line.trim());
        }

        if ctest_exit != 0 {
            self.failed_tests = summary::failed_tests(&lines);
            for line in summary::failure_report(&lines) {
                self.console.line(line);
            }
            self.console.line("");
            self.console
                .line(format!("See {} for full output.", self.log.display()));
            if self.receipt.rescue_status == "rescued"
                && let Some(dir) = self.receipt.rescued_config_dir.clone()
            {
                self.console
                    .line(format!("Test application logs rescued to {dir}"));
            }
        }

        if census_mismatch {
            self.console.line("");
            if counts.parsed {
                self.console.line(format!(
                    "Census mismatch: {expected} test(s) had to be accounted for, {accounted} were."
                ));
                self.invalid(format!(
                    "census mismatch: expected {expected} accounted tests, got {accounted}"
                ));
            } else {
                self.console.line(
                    "ctest printed no summary this run could read. Nothing was accounted for.",
                );
                self.invalid("ctest printed no parseable summary");
            }
            self.console
                .line("The suite result cannot be read as a verdict.");
        }

        // The raw ctest code wins when tests failed: it is the more specific
        // answer. The wrapper's code covers a run with no usable verdict.
        if ctest_exit != 0 {
            return Ok(ctest_exit);
        }
        if !self.receipt.invalid_reasons.is_empty() {
            return Ok(EXIT_INVALID_RUN);
        }
        Ok(0)
    }

    /// Secures what the suite wrote into the throwaway configuration
    /// directory before the cleanup deletes it. See [`rescue::rescue`].
    fn save_evidence(&mut self, verdict: Option<i32>) {
        let Some(config_dir) = self.config_dir.clone() else {
            return;
        };
        let destination = self.log_dir.join("last-run-config-dir").join(&self.run_id);
        match rescue::rescue(&config_dir, &destination, verdict) {
            Rescue::NotNeeded => {}
            Rescue::Rescued { files } => {
                self.receipt.rescued_config_dir = Some(destination.display().to_string());
                self.receipt.rescue_status = "rescued";
                self.receipt.rescue_detail = Some(format!("{files} file(s)"));
            }
            Rescue::Failed {
                detail,
                reason,
                message,
            } => {
                // The original is now the only copy: the cleanup must keep it.
                self.keep_config_dir = true;
                let shown = config_dir.display().to_string();
                self.receipt.rescued_config_dir = Some(shown.clone());
                self.receipt.rescue_status = "failed";
                self.receipt.rescue_detail = Some(detail);
                self.invalid(format!("test evidence could not be secured: {reason}"));
                self.console.line(message);
                self.console
                    .line(format!("The original directory is kept at {shown}"));
            }
        }
    }

    /// Closes the receipt and publishes it, with the tree still held: the
    /// closing fingerprint would otherwise be taken against a tree another run
    /// may already own. Returns the exit code to report.
    fn complete(&mut self, exit_code: i32) -> i32 {
        let mut exit_code = exit_code;
        if let Err(error) = self.host.checkpoint(Checkpoint::BeforeReceipt) {
            self.invalid(format!("run aborted: {error:#}"));
        }
        self.completed = true;
        self.receipt.finished_utc = Some(receipt::utc_now());
        self.receipt.exit_code = exit_code;

        if let Some(before) = self
            .receipt
            .source_before
            .as_ref()
            .and_then(SourceIdentity::fingerprint)
            .map(str::to_string)
        {
            let after = fingerprint::identify(&self.options.repo_root);
            self.receipt.source_after = Some(after.clone());
            match &after {
                SourceIdentity::Unavailable { reason } => {
                    self.receipt.source_drift = None;
                    self.invalid(format!("source identity could not be re-taken: {reason}"));
                    self.console.line(format!(
                        "Cannot confirm the source did not move during the run: {reason}"
                    ));
                }
                SourceIdentity::Known { fingerprint, .. } => {
                    let drift = *fingerprint != before;
                    self.receipt.source_drift = Some(drift);
                    if drift {
                        self.invalid("the working tree changed while the run was in progress");
                        self.console.line(
                            "The source changed while this run was in progress. Its result describes neither state.",
                        );
                    }
                }
            }
        }

        let record = &mut self.receipt;
        record.reusable = exit_code == 0
            && record.invalid_reasons.is_empty()
            && record.freshness == Freshness::Fresh
            && record.source_drift == Some(false)
            && !record.census_mismatch;

        // A drift or a failed re-take found after the suite ran still has to
        // change the verdict, or the receipt would carry a reason nobody acts on.
        if exit_code == 0 && !record.invalid_reasons.is_empty() {
            exit_code = EXIT_INVALID_RUN;
            record.exit_code = exit_code;
        }

        if self.log_dir.is_dir() {
            match receipt::publish(&self.receipt_path, &self.receipt) {
                Ok(()) => {
                    self.published = true;
                    self.console
                        .line(format!("Receipt: {}", self.receipt_path.display()));
                }
                Err(message) => {
                    self.console.line(format!(
                        "Could not publish the run receipt to {} : {message}",
                        self.receipt_path.display()
                    ));
                    // The run may have been fine. The record of it is what
                    // failed, and nothing downstream may read that as a pass.
                    if exit_code == 0 {
                        exit_code = EXIT_INVALID_RUN;
                    }
                }
            }
            self.console.line(SEPARATOR);
        }
        exit_code
    }

    fn remove_config_dir(&mut self) {
        let Some(config_dir) = &self.config_dir else {
            return;
        };
        if self.keep_config_dir {
            return;
        }
        if let Err(error) = std::fs::remove_dir_all(config_dir) {
            // Not fatal, but not silent: the next reader will find it.
            self.console.line(format!(
                "Could not remove the throwaway config dir {} : {error}",
                config_dir.display()
            ));
        }
    }

    /// The environment of every child of this run: the caller's, the MSVC
    /// import, Qt in front of PATH, offscreen Qt, the throwaway configuration
    /// directory, and the marks of the locks held while the child runs.
    fn env_for(&self, marks: &[(String, String)]) -> ChildEnv {
        let mut env = ChildEnv(self.options.child_env.clone());
        env.extend(self.msvc.iter().cloned());
        if let Some(bin) = &self.qt.bin {
            env.prepend_path(bin);
        }
        if let Some(plugins) = &self.qt.plugins {
            env.set("QT_PLUGIN_PATH", plugins);
        }
        env.set("QT_QPA_PLATFORM", "offscreen");
        if let Some(config_dir) = &self.config_dir {
            env.set("EXOSNAP_CONFIG_DIR", config_dir);
        }
        env.extend(self.host.lock_env());
        env.extend(self.tree_mark.iter().cloned());
        env.extend(marks.iter().cloned());
        env
    }

    fn name_list(&mut self, heading: &str, names: &[String]) {
        const LIMIT: usize = 12;
        if names.is_empty() {
            return;
        }
        self.console.line(format!("      {heading}"));
        for name in names.iter().take(LIMIT) {
            self.console.line(format!("        {name}"));
        }
        if names.len() > LIMIT {
            self.console
                .line(format!("        ... and {} more", names.len() - LIMIT));
        }
    }
}
