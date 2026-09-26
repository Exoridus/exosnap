//! Host-wide locks shared with `scripts/lib/HostResourceLock.psm1`.
//!
//! The names, the tree key, the inherit variables and the timeout and namespace
//! variables must stay identical to the PowerShell module while any PowerShell
//! holder remains (run-tests.ps1 takes the tree and device locks itself). The
//! protocol is described there: lock order tree, then build, then device; a named
//! mutex in the Global namespace, released by the OS when its holder dies; a
//! holder marks its children's environment so a child does not wait on its own
//! parent.

use std::path::Path;
use std::time::Duration;

use sha2::{Digest, Sha256};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LockKind {
    Tree,
    Build,
}

fn namespace() -> String {
    match std::env::var("EXOSNAP_HOST_LOCK_NAMESPACE") {
        Ok(value) if !value.trim().is_empty() => format!("ExoSnap.Host.{value}"),
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
    match kind {
        LockKind::Tree => format!(
            "Global\\{}.Tree.{}",
            namespace(),
            tree_key(tree.expect("a tree lock needs its build directory"))
        ),
        LockKind::Build => format!("Global\\{}.Build", namespace()),
    }
}

pub fn inherit_variable(kind: LockKind, tree: Option<&Path>) -> String {
    match kind {
        LockKind::Tree => format!(
            "EXOSNAP_HOST_LOCK_TREE_{}",
            tree_key(tree.expect("a tree lock needs its build directory"))
        ),
        LockKind::Build => "EXOSNAP_HOST_LOCK_BUILD".to_string(),
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
    platform::acquire(&mut lock, &lock_name(kind, tree), timeout(), tree)?;
    Ok(lock)
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
    fn the_job_budget_prefers_a_positive_override() {
        assert_eq!(job_budget(Some("3")), 3);
        assert!(job_budget(Some("0")) >= 1);
        assert!(job_budget(Some("x")) >= 1);
        assert!(job_budget(None) >= 1);
    }

    /// The PowerShell module still holds these locks from run-tests.ps1. Both
    /// sides must name the same mutex for the same directory, or a verify build
    /// and a test run stop excluding each other without any error.
    #[cfg(windows)]
    #[test]
    fn the_names_match_the_powershell_module() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let module = repo.join("scripts/lib/HostResourceLock.psm1");
        let tree = repo.join("build/windows-x64-ninja-debug");
        let script = format!(
            "Import-Module '{}' -Force; Get-HostLockName -Kind tree -Path '{}'; Get-HostLockName -Kind build",
            module.display(),
            tree.display()
        );
        let output = std::process::Command::new("pwsh")
            .args(["-NoProfile", "-NonInteractive", "-Command", &script])
            .output();
        let Ok(output) = output else {
            panic!("pwsh is required for the interop check while PowerShell lock holders exist");
        };
        let text = String::from_utf8_lossy(&output.stdout);
        let lines: Vec<&str> = text
            .lines()
            .map(str::trim)
            .filter(|l| !l.is_empty())
            .collect();
        assert_eq!(
            lines,
            vec![
                lock_name(LockKind::Tree, Some(&tree)),
                lock_name(LockKind::Build, None)
            ]
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
}
