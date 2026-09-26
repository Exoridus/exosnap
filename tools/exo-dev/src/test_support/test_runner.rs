//! Fixtures for the `exo-dev test` runner: a host that records which locks
//! are held when, and CTest trees the real ctest can list and run.
//!
//! Every test command in these trees is CMake itself (`cmake -E true`,
//! `cmake -E false`, or `cmake -P <script>`), so the same fixtures run on
//! Linux and on Windows.

use std::collections::BTreeMap;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::host_lock::{self, HostLock, LockKind, LockSpace};
use crate::test::host::{Checkpoint, HeldLock, Host};
use crate::test::runner::Options;

type Hook = Box<dyn Fn(Checkpoint, &RecordingHost) -> anyhow::Result<()>>;

#[derive(Default)]
struct State {
    timeline: Vec<String>,
    held: Vec<LockKind>,
}

/// A [`Host`] that records every lock acquisition, release, MSVC import and
/// checkpoint in order, and holds real named mutexes only when asked to.
#[derive(Default)]
pub struct RecordingHost {
    state: Arc<Mutex<State>>,
    /// What [`Host::var`] answers. Nothing falls through to the process
    /// environment, so a developer's own Qt install cannot change a result.
    pub vars: BTreeMap<String, OsString>,
    /// Kinds whose acquisition fails as if another run held them, each for
    /// one build tree only or (`None`) for every one.
    pub contended: Vec<(LockKind, Option<PathBuf>)>,
    /// Real locks in this space instead of recorded ones.
    pub real_locks: Option<LockSpace>,
    pub msvc: Option<Vec<(String, String)>>,
    pub msvc_error: Option<String>,
    pub fault_at: Option<Checkpoint>,
    hook: Option<Hook>,
}

impl RecordingHost {
    /// Qt resolves to an install root that does not exist, which a run
    /// reports as a warning and carries on.
    pub fn new() -> RecordingHost {
        let mut host = RecordingHost::default();
        host.vars.insert(
            "EXOSNAP_QT_ROOT".into(),
            std::env::temp_dir()
                .join("exo-dev-no-such-qt-root")
                .into_os_string(),
        );
        host
    }

    /// Runs `hook` at every checkpoint, after it is recorded. An error it
    /// returns is the checkpoint's result.
    pub fn on_checkpoint(
        mut self,
        hook: impl Fn(Checkpoint, &RecordingHost) -> anyhow::Result<()> + 'static,
    ) -> RecordingHost {
        self.hook = Some(Box::new(hook));
        self
    }

    pub fn timeline(&self) -> Vec<String> {
        self.state.lock().unwrap().timeline.clone()
    }

    /// The kinds held right now, in acquisition order.
    pub fn held(&self) -> Vec<LockKind> {
        self.state.lock().unwrap().held.clone()
    }

    fn record(&self, event: String) {
        self.state.lock().unwrap().timeline.push(event);
    }
}

fn kind_name(kind: LockKind) -> String {
    format!("{kind:?}").to_lowercase()
}

struct RecordedLock {
    kind: LockKind,
    variable: String,
    state: Arc<Mutex<State>>,
    real: Option<HostLock>,
}

impl HeldLock for RecordedLock {
    fn child_env(&self) -> (String, String) {
        match &self.real {
            Some(real) => real.child_env(),
            None => (self.variable.clone(), "recording host".into()),
        }
    }

    fn waited(&self) -> Duration {
        self.real.as_ref().map_or(Duration::ZERO, HostLock::waited)
    }
}

impl Drop for RecordedLock {
    fn drop(&mut self) {
        // The real mutex, if any, is released first, so a reader of the
        // timeline never sees a release that has not happened yet.
        drop(self.real.take());
        let mut state = self.state.lock().unwrap();
        state
            .timeline
            .push(format!("release {}", kind_name(self.kind)));
        if let Some(index) = state.held.iter().rposition(|k| *k == self.kind) {
            state.held.remove(index);
        }
    }
}

impl Host for RecordingHost {
    fn acquire(
        &self,
        kind: LockKind,
        tree: Option<&Path>,
        holder: &str,
    ) -> anyhow::Result<Box<dyn HeldLock>> {
        let refused = self
            .contended
            .iter()
            .any(|(k, path)| *k == kind && path.as_deref().is_none_or(|p| tree == Some(p)));
        if refused {
            self.record(format!("refuse {}", kind_name(kind)));
            anyhow::bail!(
                "host lock '{}' is held by another run and was not released within 2 s",
                host_lock::lock_name(kind, tree)
            );
        }
        let real = match &self.real_locks {
            Some(space) => Some(space.acquire(kind, tree, holder)?),
            None => None,
        };
        let mut state = self.state.lock().unwrap();
        state.timeline.push(format!("acquire {}", kind_name(kind)));
        state.held.push(kind);
        Ok(Box::new(RecordedLock {
            kind,
            variable: host_lock::inherit_variable(kind, tree),
            state: Arc::clone(&self.state),
            real,
        }))
    }

    fn lock_env(&self) -> Vec<(String, String)> {
        self.real_locks
            .as_ref()
            .map(LockSpace::child_env)
            .unwrap_or_default()
    }

    fn msvc_environment(&self) -> anyhow::Result<Option<Vec<(String, String)>>> {
        self.record("msvc".into());
        if let Some(error) = &self.msvc_error {
            anyhow::bail!("{error}");
        }
        Ok(self.msvc.clone())
    }

    fn var(&self, name: &str) -> Option<OsString> {
        self.vars.get(name).cloned()
    }

    fn checkpoint(&self, point: Checkpoint) -> anyhow::Result<()> {
        self.record(format!("{point:?}"));
        if let Some(hook) = &self.hook {
            hook(point, self)?;
        }
        if self.fault_at == Some(point) {
            anyhow::bail!("injected fault at {point:?}");
        }
        Ok(())
    }
}

/// A path as CMake wants it inside a quoted argument: forward slashes, since
/// a backslash is an escape there.
pub fn cmake_path(path: &Path) -> String {
    path.display().to_string().replace('\\', "/")
}

/// The absolute path of `program` on PATH. Test commands name it absolutely:
/// ctest resolves a command against the environment it inherits, which a run
/// changes.
pub fn on_path(program: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    let names: Vec<String> = if cfg!(windows) {
        vec![format!("{program}.exe"), program.to_string()]
    } else {
        vec![program.to_string()]
    };
    std::env::split_paths(&path)
        .flat_map(|dir| names.iter().map(move |name| dir.join(name)))
        .find(|candidate| candidate.is_file())
}

pub fn cmake() -> String {
    cmake_path(&on_path("cmake").expect("the runner contract tests need cmake on PATH"))
}

/// One test registration line pair for a hand-written CTestTestfile.cmake.
pub fn add_test(name: &str, labels: &str, args: &[&str]) -> String {
    let quoted: Vec<String> = args.iter().map(|a| format!("\"{a}\"")).collect();
    let mut text = format!("add_test({name} \"{}\" {})\n", cmake(), quoted.join(" "));
    if !labels.is_empty() {
        text.push_str(&format!(
            "set_tests_properties({name} PROPERTIES LABELS \"{labels}\")\n"
        ));
    }
    text
}

/// A test that passes.
pub fn passing(name: &str, labels: &str) -> String {
    add_test(name, labels, &["-E", "true"])
}

/// A test that fails.
pub fn failing(name: &str, labels: &str) -> String {
    add_test(name, labels, &["-E", "false"])
}

/// A test that runs a CMake script, with `defines` as `-D` arguments.
pub fn scripted(name: &str, labels: &str, script: &Path, defines: &[(&str, &Path)]) -> String {
    let mut args: Vec<String> = defines
        .iter()
        .map(|(key, value)| format!("-D{key}={}", cmake_path(value)))
        .collect();
    args.push("-P".into());
    args.push(cmake_path(script));
    let refs: Vec<&str> = args.iter().map(String::as_str).collect();
    add_test(name, labels, &refs)
}

/// Writes a CMake script. Paths inside it must come from [`cmake_path`].
pub fn script(dir: &Path, name: &str, body: &str) -> PathBuf {
    let path = dir.join(name);
    std::fs::write(&path, body).expect("write fixture script");
    path
}

/// A script that writes its whole environment to `${OUT}`.
pub fn env_dump_script(dir: &Path) -> PathBuf {
    script(
        dir,
        "dump-env.cmake",
        "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E environment OUTPUT_FILE \"${OUT}\")\n",
    )
}

/// `NAME=value` lines as written by [`env_dump_script`].
pub fn read_env_dump(path: &Path) -> BTreeMap<String, String> {
    let text = std::fs::read_to_string(path)
        .unwrap_or_else(|e| panic!("no environment dump at {}: {e}", path.display()));
    text.lines()
        .filter_map(|line| line.split_once('='))
        .map(|(k, v)| {
            let key = if cfg!(windows) {
                k.to_uppercase()
            } else {
                k.to_string()
            };
            (key, v.to_string())
        })
        .collect()
}

/// A build tree made of a hand-written CTestTestfile.cmake: ctest lists and
/// runs it, but nothing can build it.
pub fn hand_written_tree(body: &str) -> tempfile::TempDir {
    let dir = tempfile::tempdir().expect("create fixture tree");
    std::fs::write(dir.path().join("CTestTestfile.cmake"), body).expect("write CTestTestfile");
    dir
}

/// The three-test tree most selection cases use: one hardware test labelled
/// `live`, one script suite labelled `live_verify`, one with only a phase.
pub fn labelled_tree() -> tempfile::TempDir {
    hand_written_tree(&format!(
        "{}{}{}",
        passing("fixture.hardware", "live;phase.gpu"),
        passing("fixture.script_suite", "live_verify;phase.hermetic"),
        passing("fixture.unlabelled", "phase.hermetic"),
    ))
}

/// A real CMake project with no languages, configured with Ninja: a tree a
/// build system answers for, which a hand-written one never is. Returns the
/// root holding `src/` and `build/`, and the build directory.
pub fn configured_tree(extra_cmake: &str) -> (tempfile::TempDir, PathBuf) {
    let root = tempfile::tempdir().expect("create configured fixture");
    let source = root.path().join("src");
    let build = root.path().join("build");
    std::fs::create_dir_all(&source).unwrap();
    std::fs::write(
        source.join("CMakeLists.txt"),
        format!(
            "cmake_minimum_required(VERSION 3.20)\n\
             project(run_tests_fixture NONE)\n\
             enable_testing()\n\
             add_test(NAME fixture.trivial COMMAND \"${{CMAKE_COMMAND}}\" -E true)\n\
             set_tests_properties(fixture.trivial PROPERTIES LABELS \"phase.hermetic\")\n\
             {extra_cmake}\n"
        ),
    )
    .unwrap();
    let mut command = crate::process::command("cmake");
    command
        .arg("-S")
        .arg(&source)
        .arg("-B")
        .arg(&build)
        .args(["-G", "Ninja"]);
    if let Some(ninja) = ninja() {
        command.arg(format!("-DCMAKE_MAKE_PROGRAM={}", cmake_path(&ninja)));
    }
    let output = command
        .output()
        .expect("run cmake to configure the fixture");
    assert!(
        output.status.success(),
        "the fixture project did not configure (the runner contract tests need cmake and ninja):\n{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    (root, build)
}

/// ninja on PATH, else the copy Visual Studio's CMake component ships.
fn ninja() -> Option<PathBuf> {
    on_path("ninja").or_else(|| {
        let installation = crate::msvc::find_installation()?;
        let bundled =
            installation.join("Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe");
        bundled.is_file().then_some(bundled)
    })
}

/// A committed repository the run identifies its source from, with the
/// `.qt-version` every run reads.
pub fn source_repo() -> tempfile::TempDir {
    super::fixture_repo_committed(&[(".qt-version", "6.11.2\n")])
}

/// Options for a run of `build_dir` against `repo`, skipping the build and
/// accepting an unproven tree: what a hand-written tree needs to reach the
/// suite at all.
pub fn options(repo: &Path, build_dir: &Path) -> Options {
    Options {
        jobs: 2,
        no_build: true,
        allow_stale: true,
        build_dir: build_dir.to_path_buf(),
        ..Options::new(repo)
    }
}
