//! What each step actually runs.

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::path::{Path, PathBuf};

use crate::evidence;
use crate::host_lock::{self, HostLock, LockKind};
use crate::msvc;
use crate::plan::{Check, Event, Plan};
use crate::process::{StepExit, StepRunner, tail};
use crate::profile::ProfileSpec;
use crate::run::{Executor, Outcome, Status};
use crate::step::StepId;

/// check-quality.ps1's "a tool this run needed is not installed".
const QUALITY_TOOL_MISSING_EXIT: i32 = 3;

/// A native check's own signal that the external tool it needs is not
/// installed, distinct from an ordinary failure. `native` downcasts to this
/// type and maps it to `Status::ToolMissing`; every other error becomes
/// `Status::Fail`.
#[derive(Debug)]
pub struct ToolMissing(pub String);

impl std::fmt::Display for ToolMissing {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ToolMissing {}

pub struct Context {
    pub repo_root: PathBuf,
    pub profile: ProfileSpec,
    pub event: Event,
    pub base: Option<String>,
    pub staged: bool,
    pub preset: String,
    pub config: String,
    pub build_dir: String,
    pub jobs: usize,
    pub pr_title: Option<String>,
    pub pr_number: Option<String>,
    pub tidy_cache_dir: Option<PathBuf>,
    pub head: Option<String>,
    pub dirty: bool,
    pub runner: StepRunner,
}

impl Context {
    fn script(&self, name: &str) -> String {
        self.repo_root
            .join("scripts")
            .join(name)
            .display()
            .to_string()
    }

    fn tree(&self) -> PathBuf {
        self.repo_root.join(&self.build_dir)
    }
}

/// How a process step is run and judged.
#[derive(Clone, Copy, Default)]
struct Opts<'a> {
    cwd: Option<&'a Path>,
    /// Print and record the first compiler, linker or generator error.
    first_error: bool,
    /// The exit code a script uses to say the tool it delegates to is missing.
    tool_missing_exit: Option<i32>,
    env: &'a [(String, String)],
}

pub struct RealExecutor {
    ctx: Context,
    msvc_ready: bool,
    failed_tests: Vec<String>,
    tests_unfiltered: bool,
    /// Inherit marks of the host locks the running step holds.
    lock_env: Vec<(String, String)>,
}

impl RealExecutor {
    pub fn new(ctx: Context, plan: &Plan) -> RealExecutor {
        let tests_unfiltered = plan.check(StepId::Tests).is_some_and(|c| {
            c.applicable && c.evidence_str("filter").unwrap_or_default().is_empty()
        });
        RealExecutor {
            ctx,
            msvc_ready: false,
            failed_tests: Vec::new(),
            tests_unfiltered,
            lock_env: Vec::new(),
        }
    }

    /// Runs one process step and turns its exit into an outcome, printing the log
    /// tail on failure.
    fn step(&self, name: &str, program: &str, args: Vec<String>, opts: Opts) -> Outcome {
        let cwd = opts.cwd.unwrap_or(&self.ctx.repo_root);
        let mut env = self.lock_env.clone();
        env.extend(opts.env.iter().cloned());
        let (exit, log) = match self.ctx.runner.run(name, program, &args, cwd, &env) {
            Ok(result) => result,
            Err(error) => return Outcome::fail(format!("could not run {program}: {error}")),
        };
        let log_text = log.display().to_string();
        match exit {
            StepExit::Code(0) => Outcome::pass("").with_log(&log_text),
            StepExit::NotFound => {
                let reason = format!("{program} is not installed or not on PATH");
                println!();
                println!("---- {name} ----");
                println!("{reason}");
                Outcome::new(Status::ToolMissing, reason).with_log(&log_text)
            }
            StepExit::Code(code) if Some(code) == opts.tool_missing_exit => {
                let reason = tail(&log, usize::MAX)
                    .into_iter()
                    .find(|line| line.contains("NOT_RUN"))
                    .map(|line| line.trim().to_string())
                    .unwrap_or_else(|| "the tool it delegates to is not installed".into());
                println!();
                println!("---- {name} ----");
                println!("{reason}");
                Outcome::new(Status::ToolMissing, reason).with_log(&log_text)
            }
            StepExit::Code(code) => {
                let mut detail = format!("exit {code}");
                if opts.first_error
                    && let Some(first) = evidence::first_build_error(&log)
                {
                    println!();
                    println!("---- {name} first error ----");
                    println!("{first}");
                    detail = format!("{detail}; first error: {first}");
                }
                self.ctx.runner.print_tail(name, &log);
                Outcome::fail(detail).with_log(&log_text)
            }
        }
    }

    fn pwsh(&self, name: &str, script: &str, args: &[&str]) -> Outcome {
        self.pwsh_with(
            name,
            script,
            args.iter().map(|a| a.to_string()).collect(),
            None,
            &[],
        )
    }

    fn pwsh_with(
        &self,
        name: &str,
        script: &str,
        args: Vec<String>,
        tool_missing_exit: Option<i32>,
        extra_env: &[(String, String)],
    ) -> Outcome {
        let mut argv = vec![
            "-NoProfile".to_string(),
            "-NonInteractive".to_string(),
            "-File".to_string(),
            self.ctx.script(script),
        ];
        argv.extend(args);
        self.step(
            name,
            "pwsh",
            argv,
            Opts {
                tool_missing_exit,
                env: extra_env,
                ..Opts::default()
            },
        )
    }

    /// Makes cl.exe reachable before the first step that compiles. The import
    /// applies to child processes only and is skipped when cl.exe resolves.
    fn ensure_compiler(&mut self) -> Result<(), Outcome> {
        if self.msvc_ready {
            return Ok(());
        }
        self.msvc_ready = true;
        if !msvc::preset_uses_ninja(&self.ctx.repo_root, &self.ctx.preset) {
            return Ok(());
        }
        match msvc::environment() {
            Ok(Some((compiler, variables))) => {
                println!("msvc: {}", compiler.display());
                self.ctx.runner.env = variables
                    .into_iter()
                    .map(|(k, v)| (OsString::from(k), OsString::from(v)))
                    .collect();
                Ok(())
            }
            Ok(None) => Ok(()),
            Err(error) => {
                self.msvc_ready = false;
                Err(Outcome::fail(format!("{error:#}")))
            }
        }
    }

    fn env_var(&self, name: &str) -> String {
        self.ctx
            .runner
            .env
            .iter()
            .find(|(key, _)| key.to_string_lossy().eq_ignore_ascii_case(name))
            .map(|(_, value)| value.to_string_lossy().into_owned())
            .or_else(|| std::env::var(name).ok())
            .unwrap_or_default()
    }

    /// Takes the locks a step declares, tree before build.
    fn acquire_locks(&self, check: &Check) -> Result<Vec<HostLock>, Outcome> {
        let locks = check.id.info().locks;
        let holder = format!("verify {}", self.ctx.repo_root.display());
        let tree = self.ctx.tree();
        let mut held: Vec<HostLock> = Vec::new();
        for (wanted, kind) in [(locks.tree, LockKind::Tree), (locks.build, LockKind::Build)] {
            if !wanted {
                continue;
            }
            let path = (kind == LockKind::Tree).then_some(tree.as_path());
            let lock = host_lock::acquire(kind, path, &holder)
                .map_err(|error| Outcome::fail(format!("{error:#}")))?;
            if lock.waited().as_secs() >= 2 {
                let what = match kind {
                    LockKind::Tree => format!("tree lock on {}", tree.display()),
                    LockKind::Build => "host build lock".to_string(),
                };
                println!(
                    "  waited {}s for the {what} held by another run",
                    lock.waited().as_secs()
                );
            }
            held.push(lock);
        }
        Ok(held)
    }

    fn script_tests(&self) -> Outcome {
        let dir = self.ctx.repo_root.join("scripts/tests");
        let mut suites: Vec<PathBuf> = std::fs::read_dir(&dir)
            .map(|entries| {
                entries
                    .filter_map(Result::ok)
                    .map(|e| e.path())
                    .filter(|p| p.is_file() && p.to_string_lossy().ends_with(".tests.ps1"))
                    .collect()
            })
            .unwrap_or_default();
        suites.sort();
        let mut left_to_ctest = 0;
        if self.tests_unfiltered {
            let covered = evidence::ctest_script_suites(&self.ctx.repo_root);
            let before = suites.len();
            suites.retain(|p| {
                let name = p.file_name().unwrap().to_string_lossy().into_owned();
                !covered.contains(&name)
            });
            left_to_ctest = before - suites.len();
        }
        for suite in &suites {
            let name = suite.file_name().unwrap().to_string_lossy().into_owned();
            let stem = name.trim_end_matches(".ps1");
            let outcome = self.step(
                &format!("script-tests.{stem}"),
                "pwsh",
                vec![
                    "-NoProfile".into(),
                    "-NonInteractive".into(),
                    "-File".into(),
                    suite.display().to_string(),
                ],
                Opts::default(),
            );
            if outcome.status != Status::Pass {
                let mut failed = Outcome::new(
                    if outcome.status == Status::ToolMissing {
                        Status::ToolMissing
                    } else {
                        Status::Fail
                    },
                    format!("{name} failed"),
                );
                failed.evidence = outcome.evidence;
                return failed;
            }
        }
        let mut detail = format!("{} script test file(s)", suites.len());
        if left_to_ctest > 0 {
            detail.push_str(&format!("; {left_to_ctest} left to CTest"));
        }
        Outcome::pass(detail)
    }

    fn rust(&self) -> Outcome {
        let tools = self.ctx.repo_root.join("tools");
        let steps: [(&str, &[&str]); 3] = [
            ("rust.format", &["fmt", "--all", "--check"]),
            (
                "rust.clippy",
                &[
                    "clippy",
                    "--workspace",
                    "--all-targets",
                    "--locked",
                    "--",
                    "-D",
                    "warnings",
                ],
            ),
            ("rust.test", &["test", "--workspace", "--locked"]),
        ];
        for (name, args) in steps {
            let outcome = self.step(
                name,
                "cargo",
                args.iter().map(|a| a.to_string()).collect(),
                Opts {
                    cwd: Some(&tools),
                    ..Opts::default()
                },
            );
            if outcome.status != Status::Pass {
                let mut failed = outcome;
                failed.detail = format!("{name}: {}", failed.detail);
                return failed;
            }
        }
        Outcome::pass("format, clippy, tests")
    }

    /// Runs an in-process (native) check. Writes the step's log to the same
    /// place a spawned process's would land, so `--failure-tail-lines` and the
    /// receipt's evidence work unchanged; streams that log inside a
    /// `::group::`/`::endgroup::` pair in CI, matching how `StepRunner::run`
    /// streams a child process's output; and turns `f`'s result into an
    /// `Outcome` that can never pass silently.
    ///
    /// `f` returns the text to write to the step's log (typically the rendered
    /// violations or findings) alongside the `Outcome` to report. An `Err`
    /// becomes `Status::Fail`, printing the same `---- {name} ----` / reason
    /// block the process path prints for a missing tool -- except when the
    /// error downcasts to `ToolMissing`, which instead becomes
    /// `Status::ToolMissing`: a required tool that is absent is never
    /// conflated with an ordinary failure.
    fn native(&self, name: &str, f: impl FnOnce() -> anyhow::Result<(String, Outcome)>) -> Outcome {
        let (log_text, mut outcome) = match f() {
            Ok(pair) => pair,
            Err(error) => match error.downcast::<ToolMissing>() {
                Ok(missing) => {
                    let reason = missing.0;
                    println!();
                    println!("---- {name} ----");
                    println!("{reason}");
                    (reason.clone(), Outcome::new(Status::ToolMissing, reason))
                }
                Err(error) => {
                    let detail = format!("{error:#}");
                    (detail.clone(), Outcome::fail(detail))
                }
            },
        };

        let log_path = self.ctx.runner.log_path(name);
        match std::fs::create_dir_all(&self.ctx.runner.log_dir)
            .and_then(|()| std::fs::write(&log_path, &log_text))
        {
            Ok(()) => outcome = outcome.with_log(&log_path.display().to_string()),
            Err(error) => eprintln!("exo-dev: could not write {}: {error}", log_path.display()),
        }

        if self.ctx.runner.stream {
            println!("::group::{name}");
            for line in log_text.lines() {
                println!("{line}");
            }
            println!("::endgroup::");
        }
        if outcome.status == Status::Fail {
            self.ctx.runner.print_tail(name, &log_path);
        }
        outcome
    }
}

impl RealExecutor {
    fn dispatch(&mut self, check: &Check) -> Outcome {
        let ctx = &self.ctx;
        let jobs = ctx.jobs.to_string();
        match check.id {
            StepId::Sanity => {
                let mut problems = Vec::new();
                if ctx.head.is_none() {
                    problems.push("HEAD could not be resolved (not a git repository?)");
                }
                if !ctx.repo_root.join(".qt-version").is_file() {
                    problems.push(".qt-version is missing");
                }
                if !ctx.repo_root.join("CMakePresets.json").is_file() {
                    problems.push("CMakePresets.json is missing");
                }
                if !problems.is_empty() {
                    return Outcome::fail(problems.join("; "));
                }
                let head = ctx.head.as_deref().unwrap_or_default();
                let tree = if ctx.dirty {
                    "working tree dirty"
                } else {
                    "working tree clean"
                };
                Outcome::pass(format!("{}, {tree}", &head[..head.len().min(8)]))
            }
            StepId::Diff => {
                let git = crate::git::Git::new(&ctx.repo_root);
                let mut ranges: Vec<Vec<String>> = vec![
                    vec!["diff".into(), "--check".into()],
                    vec!["diff".into(), "--cached".into(), "--check".into()],
                ];
                if ctx.profile.complete()
                    && let Some(base) = &ctx.base
                {
                    ranges.push(vec![
                        "diff".into(),
                        "--check".into(),
                        format!("{base}...HEAD"),
                    ]);
                }
                for range in ranges {
                    let args: Vec<&str> = range.iter().map(String::as_str).collect();
                    if git.run(&args).0 != 0 {
                        return Outcome::fail(format!(
                            "git {} reported whitespace or conflict-marker damage",
                            range.join(" ")
                        ));
                    }
                }
                Outcome::pass("")
            }
            StepId::Drift => self.native("drift", || {
                let report = crate::drift::check(&ctx.repo_root)?;
                let mut log = String::new();
                for v in &report.violations {
                    let where_ = if v.line > 0 {
                        format!("{}:{}", v.file, v.line)
                    } else {
                        v.file.clone()
                    };
                    log.push_str(&format!("[{}] {where_}: {}\n", v.rule, v.message));
                }
                let outcome = if report.violations.is_empty() {
                    log.push_str(
                        "check-drift: OK (Qt version, Qt setup, Qt SDK paths, no qmake project files)\n",
                    );
                    Outcome::pass("")
                } else {
                    Outcome::fail(format!("{} violation(s)", report.violations.len()))
                };
                Ok((log, outcome))
            }),
            StepId::SourceHygiene => {
                let args: Vec<String> = match check.evidence_str("scope") {
                    Some("whole-tree") => vec!["-All".into()],
                    Some("range") => match &ctx.base {
                        Some(base) => vec!["-Base".into(), base.clone()],
                        None => return Outcome::fail("a pull request run needs --base"),
                    },
                    _ => vec!["-Base".into(), "HEAD".into()],
                };
                self.pwsh_with(
                    "source-hygiene",
                    "check-source-hygiene.ps1",
                    args,
                    None,
                    &[],
                )
            }
            StepId::DocsSuperpowersRemoved => self.pwsh(
                "docs-superpowers-removed",
                "check-docs-superpowers-removed.ps1",
                &[],
            ),
            StepId::CommitPolicy => {
                if !ctx.profile.ci {
                    return self.pwsh("commit-policy", "check-commit-policy.ps1", &[]);
                }
                let Some(title) = &ctx.pr_title else {
                    return Outcome::fail("a pull request run needs --pr-title");
                };
                let mut args = vec!["-Subject".to_string(), title.clone()];
                if let Some(number) = &ctx.pr_number {
                    args.extend(["-PullRequestNumber".into(), number.clone()]);
                }
                args.extend(["-Only".into(), "commit-subject".into()]);
                self.pwsh_with("commit-policy", "check-commit-policy.ps1", args, None, &[])
            }
            StepId::LintCanaries => self.pwsh("lint-canaries", "check-lint-canaries.ps1", &[]),
            StepId::ProseLines => {
                let mut args = vec!["-Advisory".to_string()];
                if ctx.profile.ci
                    && let Some(base) = &ctx.base
                {
                    args.extend(["-Base".into(), base.clone()]);
                }
                self.pwsh_with("prose-lines", "check-prose-lines.ps1", args, None, &[])
            }
            StepId::Format => {
                let args: &[&str] = if ctx.staged {
                    &["-Staged", "-Fix"]
                } else {
                    &[]
                };
                self.pwsh("format", "check-format.ps1", args)
            }
            StepId::PackagingVersion => {
                self.pwsh("packaging-version", "check-packaging-version.ps1", &[])
            }
            StepId::MsiHarvest => self.pwsh("msi-harvest", "validate-msi-harvest.ps1", &[]),
            StepId::PrivacyAllowlist => {
                self.pwsh("privacy-allowlist", "validate-privacy-allowlist.ps1", &[])
            }
            StepId::NetworkEgress => {
                self.pwsh("network-egress", "validate-network-egress.ps1", &[])
            }
            StepId::Actionlint => {
                self.step("actionlint", "actionlint", Vec::new(), Opts::default())
            }
            StepId::Zizmor => self.step(
                "zizmor",
                "zizmor",
                ["--offline", "--min-severity", "high", ".github/workflows"]
                    .map(String::from)
                    .to_vec(),
                Opts::default(),
            ),
            StepId::ScriptTests => self.script_tests(),
            StepId::Rust => self.rust(),
            StepId::Configure => {
                let mut args = vec!["--preset".to_string(), self.ctx.preset.clone()];
                args.extend(
                    self.ctx
                        .profile
                        .configure_args
                        .iter()
                        .map(|a| a.to_string()),
                );
                self.step(
                    "configure",
                    "cmake",
                    args,
                    Opts {
                        first_error: true,
                        ..Opts::default()
                    },
                )
            }
            StepId::QmlLint => {
                let args = [
                    "--build",
                    &self.ctx.build_dir,
                    "--target",
                    "all_qmllint",
                    "--parallel",
                    &jobs,
                ]
                .map(String::from)
                .to_vec();
                self.step(
                    "qmllint",
                    "cmake",
                    args,
                    Opts {
                        first_error: true,
                        ..Opts::default()
                    },
                )
            }
            StepId::Build => {
                let args = ["--build", "--preset", &self.ctx.preset, "--parallel", &jobs]
                    .map(String::from)
                    .to_vec();
                self.step(
                    "build",
                    "cmake",
                    args,
                    Opts {
                        first_error: true,
                        ..Opts::default()
                    },
                )
            }
            StepId::Tests => {
                let mut args = vec![
                    "-BuildDir".to_string(),
                    ctx.build_dir.clone(),
                    "-Config".into(),
                    ctx.config.clone(),
                    "-Jobs".into(),
                    jobs,
                ];
                if let Some(filter) = check.evidence_str("filter").filter(|f| !f.is_empty()) {
                    args.extend(["-Filter".into(), filter.to_string()]);
                }
                if let Some(label) = check.evidence_str("excludeLabel") {
                    args.extend(["-ExcludeLabel".into(), label.to_string()]);
                }
                let mut outcome = self.pwsh_with("tests", "run-tests.ps1", args, None, &[]);
                if outcome.status == Status::Fail {
                    if let Some(log) = outcome.evidence.get("log").and_then(|v| v.as_str()) {
                        self.failed_tests = evidence::failed_ctest_names(Path::new(log));
                    }
                    outcome.detail = format!(
                        "{}; failed: {}",
                        outcome.detail,
                        self.failed_tests.join(", ")
                    );
                }
                outcome
            }
            StepId::CppCheck => self.pwsh_with(
                "cppcheck",
                "check-quality.ps1",
                vec!["-Only".into(), "cppcheck".into()],
                Some(QUALITY_TOOL_MISSING_EXIT),
                &[],
            ),
            StepId::ClangTidy => {
                // The compiler import has run (see `execute`): the fingerprint
                // reads the toolset variables it exports, and an unqualified
                // fingerprint would put two toolchains in one directory.
                let cache = self.ctx.tidy_cache_dir.clone().unwrap_or_else(|| {
                    evidence::tool_cache_dir(
                        "clang-tidy",
                        &[
                            self.ctx.preset.clone(),
                            self.env_var("VCToolsVersion"),
                            self.env_var("VSCMD_ARG_HOST_ARCH"),
                            self.env_var("VSCMD_ARG_TGT_ARCH"),
                        ],
                        None,
                    )
                });
                let mut args = vec![
                    "-BuildDir".to_string(),
                    self.ctx.build_dir.clone(),
                    "-CacheDir".into(),
                    cache.display().to_string(),
                    "-Jobs".into(),
                    jobs,
                ];
                match check.evidence_str("scope") {
                    Some("changed-since-parent") => args.extend(["-Base".into(), "HEAD^".into()]),
                    Some("changed") => {
                        if let Some(base) = &self.ctx.base {
                            args.extend(["-Base".into(), base.clone()]);
                        }
                    }
                    _ => {}
                }
                self.pwsh_with("clang-tidy", "run-clang-tidy-blocking.ps1", args, None, &[])
            }
            StepId::PackagingSmoke => self.pwsh_with(
                "packaging-smoke",
                "build-release-artifacts.ps1",
                ["-SkipConfigure", "-Preset", &ctx.preset, "-SkipMsi"]
                    .map(String::from)
                    .to_vec(),
                None,
                &[],
            ),
            StepId::AvSyncGolden => self.step(
                "av-sync-golden",
                "python",
                [
                    "scripts/dev/av-sync-check.py",
                    "tests/fixtures/av-sync/clapper-golden.mp4",
                    "--max-drift-ms",
                    "25",
                    "--expected-markers",
                    "5",
                    "--json",
                ]
                .map(String::from)
                .to_vec(),
                Opts::default(),
            ),
        }
    }
}

impl Executor for RealExecutor {
    /// Runs a step under the host locks it declares. Locking is driven by the step
    /// table, not by each step's code, so a step that writes the build tree
    /// cannot forget it.
    fn execute(&mut self, check: &Check) -> Outcome {
        // A step that starts compiler processes needs cl.exe. The import runs
        // before the locks so it does not lengthen their hold.
        if check.id.info().locks.build
            && let Err(outcome) = self.ensure_compiler()
        {
            return outcome;
        }
        let mut held = match self.acquire_locks(check) {
            Ok(held) => held,
            Err(outcome) => return outcome,
        };
        self.lock_env = held.iter().map(HostLock::child_env).collect();
        let outcome = self.dispatch(check);
        self.lock_env.clear();
        // Release in reverse order of acquisition.
        while held.pop().is_some() {}
        outcome
    }

    /// A failing QML test that reports only an exit code is not a test report.
    /// Re-run the QuickTest binaries behind the failing names with `-o <file>,txt`
    /// so the per-function diagnosis exists as evidence.
    fn diagnose(&mut self, _check: &Check, _outcome: &Outcome) -> Vec<String> {
        let tree = self.ctx.tree();
        let registration =
            evidence::ctest_registration(&tree, &self.ctx.config, &self.ctx.runner.env);
        if registration.is_empty() {
            println!();
            println!(
                "no CTest registration could be read from {}; the QuickTest re-run is skipped",
                tree.display()
            );
        }
        let mut produced = Vec::new();
        for command in evidence::qml_diagnostic_commands(
            &self.failed_tests,
            &registration,
            &self.ctx.runner.log_dir,
        ) {
            if !Path::new(&command.file_path).is_file() {
                continue;
            }
            let mut process = crate::process::command(&command.file_path);
            process.args(&command.arguments);
            for (key, value) in &self.ctx.runner.env {
                process.env(key, value);
            }
            let _ = crate::process::query(process);
            if command.output_path.is_file() {
                println!();
                println!("---- {} (QuickTest report) ----", command.test_name);
                for line in tail(&command.output_path, 60) {
                    println!("{line}");
                }
                produced.push(command.output_path.display().to_string());
            }
        }
        produced
    }
}

/// Reports every executed check as passed except the simulated failures. This is
/// how the orchestrator is exercised end to end without a compiler.
pub struct DryRunExecutor {
    pub simulate_fail: Vec<String>,
    pub log_dir: PathBuf,
    failed_tests: Vec<String>,
}

impl DryRunExecutor {
    pub fn new(simulate_fail: Vec<String>, log_dir: PathBuf) -> DryRunExecutor {
        DryRunExecutor {
            simulate_fail,
            log_dir,
            failed_tests: Vec::new(),
        }
    }
}

const SIMULATED_QUICK_TEST: &str = "quick.qml.record_controls";

impl Executor for DryRunExecutor {
    fn execute(&mut self, check: &Check) -> Outcome {
        if self.simulate_fail.iter().any(|name| name == check.name()) {
            // A simulated test failure carries a simulated failing test, or the
            // QML diagnostic contract would look satisfied for the wrong reason.
            if check.id == StepId::Tests {
                self.failed_tests = vec![SIMULATED_QUICK_TEST.to_string()];
            }
            return Outcome::fail("simulated failure");
        }
        Outcome::pass("simulated")
    }

    fn diagnose(&mut self, _check: &Check, _outcome: &Outcome) -> Vec<String> {
        let registration = BTreeMap::from([(
            SIMULATED_QUICK_TEST.to_string(),
            evidence::Registration {
                file_path: "record_controls_qml_tests.exe".into(),
                labels: vec!["quick".into(), evidence::QUICK_TEST_LABEL.into()],
            },
        )]);
        evidence::qml_diagnostic_commands(&self.failed_tests, &registration, &self.log_dir)
            .into_iter()
            .map(|c| c.output_path.display().to_string())
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::plan;
    use crate::profile::Profile;
    use crate::scope::Scope;

    fn executor(log_dir: &Path) -> RealExecutor {
        let scope = Scope::everything();
        let input = plan::tests::input(Profile::PrePush, &scope);
        let plan = plan::build(&input);
        let ctx = Context {
            repo_root: PathBuf::from("."),
            profile: input.profile,
            event: input.event.clone(),
            base: input.base.clone(),
            staged: input.staged,
            preset: input.preset.clone(),
            config: input.config.clone(),
            build_dir: plan.build_dir.clone(),
            jobs: 1,
            pr_title: None,
            pr_number: None,
            tidy_cache_dir: None,
            head: Some("deadbeef".into()),
            dirty: false,
            runner: StepRunner {
                log_dir: log_dir.to_path_buf(),
                stream: false,
                failure_tail_lines: 5,
                env: Vec::new(),
            },
        };
        RealExecutor::new(ctx, &plan)
    }

    #[test]
    fn a_tool_missing_error_maps_to_tool_missing_not_fail() {
        let dir = tempfile::tempdir().unwrap();
        let executor = executor(dir.path());
        let outcome = executor.native("probe", || {
            Err(ToolMissing("zizmor is not installed".into()).into())
        });
        assert_eq!(outcome.status, Status::ToolMissing);
        assert_eq!(outcome.detail, "zizmor is not installed");
    }

    #[test]
    fn any_other_error_maps_to_an_ordinary_failure() {
        let dir = tempfile::tempdir().unwrap();
        let executor = executor(dir.path());
        let outcome = executor.native("probe", || anyhow::bail!("boom"));
        assert_eq!(outcome.status, Status::Fail);
        assert!(outcome.detail.contains("boom"));
    }
}
