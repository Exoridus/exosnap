//! Host-wide locks that keep two verify or test runs from competing for the
//! same build tree, the same binaries or the same GPU/interactive desktop.
//!
//! The mutex names and the inherit-variable names are an external contract: any
//! process on the host that names one of them the same way joins the same
//! exclusion, so they must never drift once published. Lock order is tree, then
//! build, then device; each is a named mutex in the Global namespace, released
//! by the OS when its holder dies; a holder marks its children's environment so
//! a child does not wait on a lock its own ancestor already holds.

use std::path::Path;
use std::time::Duration;

use sha2::{Digest, Sha256};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LockKind {
    Tree,
    Build,
    /// The GPU and the interactive desktop: one test run, live check or VM
    /// campaign on the host at a time.
    Device,
}

/// The variable that moves every lock to a different set of names. A holder
/// that overrides the namespace per call passes it on to its children under
/// this name, so the whole process tree agrees on one set of names.
pub const NAMESPACE_VARIABLE: &str = "EXOSNAP_HOST_LOCK_NAMESPACE";

fn namespace() -> String {
    namespace_from(std::env::var(NAMESPACE_VARIABLE).ok().as_deref())
}

fn namespace_from(value: Option<&str>) -> String {
    match value {
        Some(value) if !value.trim().is_empty() => format!("ExoSnap.Host.{value}"),
        _ => "ExoSnap.Host".to_string(),
    }
}

/// The identity of one build tree: a digest of its canonical, case-folded path.
/// The directory need not exist yet; configure creates it under the lock.
pub fn tree_key(path: &Path) -> String {
    let full = std::path::absolute(path).unwrap_or_else(|_| path.to_path_buf());
    let text = full.to_string_lossy();
    let folded = text.trim_end_matches(['\\', '/']).to_lowercase();
    let digest = Sha256::digest(folded.as_bytes());
    hex::encode(digest)[..16].to_string()
}

pub fn lock_name(kind: LockKind, tree: Option<&Path>) -> String {
    lock_name_in(&namespace(), kind, tree)
}

fn lock_name_in(namespace: &str, kind: LockKind, tree: Option<&Path>) -> String {
    match kind {
        LockKind::Tree => format!(
            "Global\\{namespace}.Tree.{}",
            tree_key(tree.expect("a tree lock needs its build directory"))
        ),
        LockKind::Build => format!("Global\\{namespace}.Build"),
        LockKind::Device => format!("Global\\{namespace}.Device"),
    }
}

pub fn inherit_variable(kind: LockKind, tree: Option<&Path>) -> String {
    match kind {
        LockKind::Tree => format!(
            "EXOSNAP_HOST_LOCK_TREE_{}",
            tree_key(tree.expect("a tree lock needs its build directory"))
        ),
        LockKind::Build => "EXOSNAP_HOST_LOCK_BUILD".to_string(),
        LockKind::Device => "EXOSNAP_HOST_LOCK_DEVICE".to_string(),
    }
}

/// `EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS` when positive, else thirty minutes.
pub fn timeout() -> Duration {
    std::env::var("EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS")
        .ok()
        .and_then(|value| value.trim().parse::<f64>().ok())
        .filter(|seconds| *seconds > 0.0)
        .map_or(Duration::from_secs(30 * 60), Duration::from_secs_f64)
}

/// `EXOSNAP_VERIFY_JOBS` when positive, else every core but two (at least one), so
/// the shell and the editor keep a core each while a gate runs.
pub fn job_budget(override_value: Option<&str>) -> usize {
    if let Some(parsed) = override_value
        .and_then(|value| value.trim().parse::<usize>().ok())
        .filter(|jobs| *jobs > 0)
    {
        return parsed;
    }
    let cores = std::thread::available_parallelism().map_or(1, usize::from);
    cores.saturating_sub(2).max(1)
}

/// A held lock. Released on drop unless it was inherited from a parent.
pub struct HostLock {
    kind: LockKind,
    variable: String,
    holder: String,
    waited: Duration,
    #[cfg(windows)]
    handle: Option<windows::Win32::Foundation::HANDLE>,
}

impl HostLock {
    /// The environment a child needs to recognise this hold as its parent's.
    pub fn child_env(&self) -> (String, String) {
        (self.variable.clone(), self.holder.clone())
    }

    pub fn waited(&self) -> Duration {
        self.waited
    }

    pub fn kind(&self) -> LockKind {
        self.kind
    }
}

/// Acquires one lock, waiting up to [`timeout`]. Errors rather than proceeding
/// unlocked: a step that ran anyway would be the collision the lock prevents.
pub fn acquire(kind: LockKind, tree: Option<&Path>, holder: &str) -> anyhow::Result<HostLock> {
    LockSpace::default().acquire(kind, tree, holder)
}

/// Where locks are taken and how long a caller waits for one, per call rather
/// than only through the process environment. Every field left `None` falls
/// back to its environment variable, so the default is exactly [`acquire`].
///
/// A caller that takes real locks from inside a run that already holds the
/// real ones (a test of the locks, run by the test runner) gives itself a
/// namespace of its own: it would otherwise contend with the process running
/// it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LockSpace {
    /// Overrides `EXOSNAP_HOST_LOCK_NAMESPACE`.
    pub namespace: Option<String>,
    /// Overrides [`timeout`].
    pub timeout: Option<Duration>,
}

impl LockSpace {
    pub fn lock_name(&self, kind: LockKind, tree: Option<&Path>) -> String {
        let namespace = match &self.namespace {
            Some(value) => namespace_from(Some(value)),
            None => namespace(),
        };
        lock_name_in(&namespace, kind, tree)
    }

    /// The variables a child needs to use the same names as this space. Empty
    /// when the namespace is the one the child inherits anyway.
    pub fn child_env(&self) -> Vec<(String, String)> {
        self.namespace
            .iter()
            .map(|value| (NAMESPACE_VARIABLE.to_string(), value.clone()))
            .collect()
    }

    /// Acquires one lock in this space. An inherit mark in the environment
    /// means a process above this one holds it already, and the returned hold
    /// releases nothing.
    pub fn acquire(
        &self,
        kind: LockKind,
        tree: Option<&Path>,
        holder: &str,
    ) -> anyhow::Result<HostLock> {
        let variable = inherit_variable(kind, tree);
        let inherited = std::env::var(&variable).is_ok_and(|v| !v.trim().is_empty());
        let mut lock = HostLock {
            kind,
            variable,
            holder: holder.to_string(),
            waited: Duration::ZERO,
            #[cfg(windows)]
            handle: None,
        };
        if inherited {
            return Ok(lock);
        }
        platform::acquire(
            &mut lock,
            &self.lock_name(kind, tree),
            self.timeout.unwrap_or_else(timeout),
            tree,
        )?;
        Ok(lock)
    }
}

#[cfg(windows)]
mod platform {
    use std::path::Path;
    use std::time::{Duration, Instant};

    use windows::Win32::Foundation::{CloseHandle, WAIT_ABANDONED, WAIT_OBJECT_0};
    use windows::Win32::System::Threading::{CreateMutexW, ReleaseMutex, WaitForSingleObject};
    use windows::core::HSTRING;

    use super::{HostLock, LockKind};

    pub fn acquire(
        lock: &mut HostLock,
        name: &str,
        timeout: Duration,
        tree: Option<&Path>,
    ) -> anyhow::Result<()> {
        // SAFETY: plain Win32 calls on a handle this function owns; it is closed
        // on every path that does not hand it to the HostLock.
        unsafe {
            let handle = CreateMutexW(None, false, &HSTRING::from(name))?;
            let started = Instant::now();
            let millis = u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX - 1);
            let wait = WaitForSingleObject(handle, millis);
            // Abandoned means the previous holder died without releasing. The lock
            // is ours, and a build or test run leaves no shared state behind that
            // a new one could misread.
            if wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED {
                let _ = CloseHandle(handle);
                let contended = match (lock.kind, tree) {
                    (LockKind::Tree, Some(path)) => format!("build tree '{}'", path.display()),
                    (kind, _) => format!("{kind:?} step on this machine").to_lowercase(),
                };
                anyhow::bail!(
                    "host lock '{name}' is held by another run and was not released within {} s; \
                     a second {contended} would compete with it for the same cores, the same device \
                     or the same binaries",
                    timeout.as_secs()
                );
            }
            lock.waited = started.elapsed();
            lock.handle = Some(handle);
        }
        Ok(())
    }

    impl Drop for HostLock {
        fn drop(&mut self) {
            if let Some(handle) = self.handle.take() {
                // SAFETY: the handle is owned by this lock and released on the
                // thread that acquired it: the executor is single-threaded.
                unsafe {
                    let _ = ReleaseMutex(handle);
                    let _ = CloseHandle(handle);
                }
            }
        }
    }
}

#[cfg(not(windows))]
mod platform {
    use std::path::Path;
    use std::time::Duration;

    use super::HostLock;

    /// Build-tree steps are Windows-only, so nothing on another platform takes a
    /// host lock. Refusing is the safe answer if that ever changes.
    pub fn acquire(
        _lock: &mut HostLock,
        name: &str,
        _timeout: Duration,
        _tree: Option<&Path>,
    ) -> anyhow::Result<()> {
        anyhow::bail!("host lock '{name}' is only implemented on Windows")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_tree_key_folds_case_and_trailing_separators() {
        let a = tree_key(Path::new("C:/Repo/Build/windows-x64"));
        let b = tree_key(Path::new("c:/repo/build/windows-x64/"));
        assert_eq!(a, b);
        assert_eq!(a.len(), 16);
        assert_ne!(a, tree_key(Path::new("C:/Repo/Build/other")));
    }

    #[test]
    fn names_follow_the_shared_scheme() {
        let tree = Path::new("C:/x/build/p");
        let name = lock_name(LockKind::Tree, Some(tree));
        assert!(name.starts_with("Global\\ExoSnap.Host"), "{name}");
        assert!(name.ends_with(&format!(".Tree.{}", tree_key(tree))));
        assert!(lock_name(LockKind::Build, None).ends_with(".Build"));
        assert_eq!(
            inherit_variable(LockKind::Build, None),
            "EXOSNAP_HOST_LOCK_BUILD"
        );
        assert_eq!(
            inherit_variable(LockKind::Tree, Some(tree)),
            format!("EXOSNAP_HOST_LOCK_TREE_{}", tree_key(tree))
        );
    }

    #[test]
    fn the_device_lock_is_host_wide_and_has_its_own_inherit_mark() {
        assert!(lock_name(LockKind::Device, None).ends_with(".Device"));
        assert_eq!(
            inherit_variable(LockKind::Device, None),
            "EXOSNAP_HOST_LOCK_DEVICE"
        );
    }

    #[test]
    fn a_lock_space_names_its_own_namespace_without_the_environment() {
        let tree = Path::new("C:/x/build/p");
        let space = LockSpace {
            namespace: Some("probe-1".into()),
            timeout: None,
        };
        assert_eq!(
            space.lock_name(LockKind::Device, None),
            "Global\\ExoSnap.Host.probe-1.Device"
        );
        assert_eq!(
            space.lock_name(LockKind::Build, None),
            "Global\\ExoSnap.Host.probe-1.Build"
        );
        assert_eq!(
            space.lock_name(LockKind::Tree, Some(tree)),
            format!("Global\\ExoSnap.Host.probe-1.Tree.{}", tree_key(tree))
        );
        assert_eq!(
            space.child_env(),
            vec![(NAMESPACE_VARIABLE.to_string(), "probe-1".to_string())]
        );
        // A blank override is the default namespace, as a blank variable is.
        let blank = LockSpace {
            namespace: Some("  ".into()),
            timeout: None,
        };
        assert_eq!(
            blank.lock_name(LockKind::Device, None),
            "Global\\ExoSnap.Host.Device"
        );
        assert!(LockSpace::default().child_env().is_empty());
    }

    #[test]
    fn the_job_budget_prefers_a_positive_override() {
        assert_eq!(job_budget(Some("3")), 3);
        assert!(job_budget(Some("0")) >= 1);
        assert!(job_budget(Some("x")) >= 1);
        assert!(job_budget(None) >= 1);
    }

    /// A holder outside this process (a script, a human with Process Explorer,
    /// a future port) can only find these locks by their exact literal names.
    /// Pinned here rather than derived, so a refactor that would silently
    /// change one fails this test instead of orphaning that external holder.
    #[test]
    fn the_pinned_names_and_inherit_variables_do_not_drift() {
        let tree = Path::new("C:/x/build/p");
        assert_eq!(
            lock_name(LockKind::Build, None),
            "Global\\ExoSnap.Host.Build"
        );
        assert_eq!(
            lock_name(LockKind::Device, None),
            "Global\\ExoSnap.Host.Device"
        );
        assert_eq!(
            lock_name(LockKind::Tree, Some(tree)),
            format!("Global\\ExoSnap.Host.Tree.{}", tree_key(tree))
        );
        assert_eq!(
            inherit_variable(LockKind::Build, None),
            "EXOSNAP_HOST_LOCK_BUILD"
        );
        assert_eq!(
            inherit_variable(LockKind::Device, None),
            "EXOSNAP_HOST_LOCK_DEVICE"
        );
        assert_eq!(
            inherit_variable(LockKind::Tree, Some(tree)),
            format!("EXOSNAP_HOST_LOCK_TREE_{}", tree_key(tree))
        );
        assert_eq!(NAMESPACE_VARIABLE, "EXOSNAP_HOST_LOCK_NAMESPACE");
    }

    /// A namespace nothing else on the host uses, so a real device lock taken
    /// here cannot contend with a test run that holds the real one.
    #[cfg(windows)]
    fn private_space(timeout: Duration) -> LockSpace {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_nanos());
        LockSpace {
            namespace: Some(format!("exo-dev-test-{}-{nanos}", std::process::id())),
            timeout: Some(timeout),
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_device_lock_in_a_private_namespace_excludes_a_second_holder_until_dropped() {
        let space = private_space(Duration::from_millis(200));
        let first = space.acquire(LockKind::Device, None, "first").unwrap();
        // Another thread: a mutex is recursive for the thread that owns it.
        let contender = std::thread::spawn({
            let space = space.clone();
            move || space.acquire(LockKind::Device, None, "second").is_err()
        });
        assert!(
            contender.join().unwrap(),
            "a second holder must not get a held device lock"
        );
        drop(first);
        let again =
            std::thread::spawn(move || space.acquire(LockKind::Device, None, "third").is_ok());
        assert!(
            again.join().unwrap(),
            "a released device lock must be free again"
        );
    }

    #[cfg(windows)]
    #[test]
    fn a_second_holder_times_out_and_a_released_lock_is_free_again() {
        // A private namespace would be cleaner, but the namespace is read from the
        // process environment. A per-test tree path gives an unshared name instead.
        let dir = tempfile::tempdir().unwrap();
        let tree = dir.path().join("tree");
        let name = lock_name(LockKind::Tree, Some(&tree));
        let first = acquire(LockKind::Tree, Some(&tree), "first").unwrap();
        let contender = std::thread::spawn({
            let name = name.clone();
            let tree = tree.clone();
            move || {
                let mut lock = HostLock {
                    kind: LockKind::Tree,
                    variable: String::new(),
                    holder: "second".into(),
                    waited: Duration::ZERO,
                    handle: None,
                };
                platform::acquire(&mut lock, &name, Duration::from_millis(200), Some(&tree))
                    .is_err()
            }
        });
        assert!(
            contender.join().unwrap(),
            "a second holder must not get a held lock"
        );
        drop(first);
        let again =
            std::thread::spawn(move || acquire(LockKind::Tree, Some(&tree), "third").is_ok());
        assert!(again.join().unwrap(), "a released lock must be free again");
    }

    /// Re-invokes this test binary as a real OS process, running only the named
    /// `#[ignore]`d test, with the given extra environment variables.
    #[cfg(windows)]
    fn spawn_probe(test_name: &str, env: &[(&str, &str)]) -> std::process::Child {
        let exe = std::env::current_exe().expect("test binary path");
        let mut command = std::process::Command::new(exe);
        command.args(["--exact", test_name, "--ignored", "--nocapture"]);
        for (key, value) in env {
            command.env(key, value);
        }
        command.spawn().expect("failed to spawn the probe process")
    }

    /// A real child process (not a thread) that inherits a hold must not wait
    /// on its own parent: the deadlock this exists to prevent is a run that
    /// holds the device lock and then runs a test which itself needs the same
    /// lock.
    #[cfg(windows)]
    #[test]
    fn a_child_process_that_inherits_the_hold_does_not_wait_on_its_parent() {
        let space = private_space(Duration::from_secs(5));
        let parent = space.acquire(LockKind::Device, None, "parent").unwrap();
        let (variable, value) = parent.child_env();
        let namespace = space.namespace.clone().unwrap();
        let mut child = spawn_probe(
            "host_lock::tests::child_probe_inherits_the_hold",
            &[
                (variable.as_str(), value.as_str()),
                (NAMESPACE_VARIABLE, namespace.as_str()),
            ],
        );
        let status = child.wait().expect("the probe process must run to exit");
        assert!(status.success(), "the child did not inherit the hold");
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "run only by a_child_process_that_inherits_the_hold_does_not_wait_on_its_parent, with its inherit variable already set"]
    fn child_probe_inherits_the_hold() {
        let namespace = std::env::var(NAMESPACE_VARIABLE).ok();
        let space = LockSpace {
            namespace,
            timeout: Some(Duration::from_millis(200)),
        };
        let lock = space
            .acquire(LockKind::Device, None, "child")
            .expect("an inherited hold must not wait");
        assert!(lock.waited().is_zero(), "an inherited hold reported a wait");
    }

    /// The OS releases a named mutex when its holder process dies without
    /// releasing it, so the next holder must still be able to take the lock
    /// rather than waiting out the full timeout or erroring.
    #[cfg(windows)]
    #[test]
    fn a_lock_held_by_a_killed_process_is_free_for_the_next_holder() {
        let space = private_space(Duration::from_secs(10));
        let namespace = space.namespace.clone().unwrap();
        let mut child = spawn_probe(
            "host_lock::tests::child_probe_holds_the_device_lock_until_killed",
            &[(NAMESPACE_VARIABLE, namespace.as_str())],
        );
        // Give the child time to actually acquire the lock before killing it.
        std::thread::sleep(Duration::from_millis(500));
        child.kill().expect("failed to kill the probe process");
        child
            .wait()
            .expect("failed to reap the killed probe process");
        let lock = space
            .acquire(LockKind::Device, None, "next")
            .expect("a lock abandoned by a killed holder must still be grantable");
        drop(lock);
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "run only by a_lock_held_by_a_killed_process_is_free_for_the_next_holder, which kills it while it sleeps"]
    fn child_probe_holds_the_device_lock_until_killed() {
        let namespace = std::env::var(NAMESPACE_VARIABLE).ok();
        let space = LockSpace {
            namespace,
            timeout: Some(Duration::from_secs(10)),
        };
        let _lock = space.acquire(LockKind::Device, None, "child").unwrap();
        std::thread::sleep(Duration::from_secs(60));
    }
}
