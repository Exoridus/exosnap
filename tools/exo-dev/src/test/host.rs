//! The effects a test run has outside the processes it starts, behind one
//! trait so the orchestration can be exercised without real named mutexes,
//! a Visual Studio installation or a Qt install.

use std::ffi::OsString;
use std::path::Path;
use std::time::Duration;

use crate::host_lock::{HostLock, LockKind, LockSpace};

/// Points in a run a test can observe or fail at. Inert in a real run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Checkpoint {
    BeforeBuild,
    AfterBuild,
    BeforeSuite,
    /// The suite has run and written its logs, and its verdict is not yet
    /// recorded. A failure here is a run that threw instead of returning one.
    AfterSuiteBeforeVerdict,
    /// The verdict is final and the receipt not yet written. The tree lock is
    /// still held.
    BeforeReceipt,
}

/// A held host lock, as the runner needs it.
pub trait HeldLock {
    /// The inherit mark a child needs to run under this hold.
    fn child_env(&self) -> (String, String);
    fn waited(&self) -> Duration;
}

impl HeldLock for HostLock {
    fn child_env(&self) -> (String, String) {
        HostLock::child_env(self)
    }

    fn waited(&self) -> Duration {
        HostLock::waited(self)
    }
}

/// The effects a run has outside the processes it starts.
pub trait Host {
    /// Acquires one host lock, or explains why it could not. Released on drop.
    fn acquire(
        &self,
        kind: LockKind,
        tree: Option<&Path>,
        holder: &str,
    ) -> anyhow::Result<Box<dyn HeldLock>>;
    /// Variables every child needs to use the same lock names.
    fn lock_env(&self) -> Vec<(String, String)>;
    /// The MSVC environment for a Ninja build when cl.exe does not resolve.
    fn msvc_environment(&self) -> anyhow::Result<Option<Vec<(String, String)>>>;
    /// A variable of this process's environment.
    fn var(&self, name: &str) -> Option<OsString>;
    fn checkpoint(&self, point: Checkpoint) -> anyhow::Result<()>;
}

/// Real named mutexes, the real MSVC import and the real environment.
#[derive(Clone, Debug, Default)]
pub struct RealHost {
    pub locks: LockSpace,
}

impl Host for RealHost {
    fn acquire(
        &self,
        kind: LockKind,
        tree: Option<&Path>,
        holder: &str,
    ) -> anyhow::Result<Box<dyn HeldLock>> {
        Ok(Box::new(self.locks.acquire(kind, tree, holder)?))
    }

    fn lock_env(&self) -> Vec<(String, String)> {
        self.locks.child_env()
    }

    fn msvc_environment(&self) -> anyhow::Result<Option<Vec<(String, String)>>> {
        Ok(crate::msvc::environment()?.map(|(_, variables)| variables))
    }

    fn var(&self, name: &str) -> Option<OsString> {
        std::env::var_os(name)
    }

    fn checkpoint(&self, _point: Checkpoint) -> anyhow::Result<()> {
        Ok(())
    }
}

/// Where the run's own messages go: always collected, and echoed live for a
/// caller at a terminal.
#[derive(Debug, Default)]
pub struct Console {
    echo: bool,
    text: String,
}

impl Console {
    pub fn live() -> Console {
        Console {
            echo: true,
            text: String::new(),
        }
    }

    pub fn captured() -> Console {
        Console::default()
    }

    pub fn line(&mut self, line: impl AsRef<str>) {
        let line = line.as_ref();
        if self.echo {
            println!("{line}");
        }
        self.text.push_str(line);
        self.text.push('\n');
    }

    pub fn text(&self) -> &str {
        &self.text
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_real_host_in_a_private_namespace_passes_it_to_its_children() {
        let host = RealHost {
            locks: LockSpace {
                namespace: Some("probe".into()),
                timeout: None,
            },
        };
        assert_eq!(
            host.lock_env(),
            vec![(
                crate::host_lock::NAMESPACE_VARIABLE.to_string(),
                "probe".to_string()
            )]
        );
        assert!(RealHost::default().lock_env().is_empty());
    }

    #[cfg(not(windows))]
    #[test]
    fn a_real_host_refuses_rather_than_running_unlocked_off_windows() {
        let error = RealHost::default()
            .acquire(LockKind::Device, None, "probe")
            .err()
            .expect("a real lock off Windows must be refused");
        assert!(error.to_string().contains("only implemented on Windows"));
    }
}
