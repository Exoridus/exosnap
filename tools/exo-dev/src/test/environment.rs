//! The environment every process of a test run starts in.
//!
//! Nothing here changes this process's own environment. Each child gets the
//! variables on its command, so a run inside a long-lived caller (`exo-dev
//! verify`) leaves that caller exactly as it was on every exit path, and a
//! variable that was unset stays unset.

use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::bail;

/// The Qt installation a run puts in front of the suite, resolved from
/// `.qt-version` at test time. Test-time Qt resolution is not owned by the
/// CMake registration (no test declares an environment modification for Qt),
/// so the runner has to provide it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct QtInstall {
    /// `<root>/bin`, prepended to PATH so Qt and FFmpeg DLLs resolve.
    pub bin: Option<PathBuf>,
    /// `<root>/plugins`, when present.
    pub plugins: Option<PathBuf>,
    /// Said when the canonical install is absent: the run then relies on
    /// whatever Qt is already on PATH, which is tolerated.
    pub warning: Option<String>,
}

/// The canonical three-part Qt version. An error, never a guess, when the
/// file is missing or malformed: a caller must not build a path from it.
pub fn canonical_qt_version(repo_root: &Path) -> anyhow::Result<String> {
    let path = repo_root.join(".qt-version");
    let Ok(text) = std::fs::read_to_string(&path) else {
        bail!(
            ".qt-version is missing at '{}'. It is the canonical Qt version for this repository.",
            path.display()
        );
    };
    let version = text.trim();
    let parts: Vec<&str> = version.split('.').collect();
    let well_formed = parts.len() == 3
        && parts
            .iter()
            .all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()));
    if !well_formed {
        bail!(".qt-version contains '{version}', which is not a three-part version (e.g. 6.11.1).");
    }
    Ok(version.to_string())
}

/// `<qt_root or <system_drive>\Qt>/<version>/msvc2022_64`, and nothing else:
/// never another version's directory. `qt_root` is `EXOSNAP_QT_ROOT`, used
/// when non-empty, and `system_drive` is `SystemDrive`.
pub fn resolve_qt(
    repo_root: &Path,
    qt_root: Option<&OsStr>,
    system_drive: Option<&OsStr>,
) -> anyhow::Result<QtInstall> {
    let version = canonical_qt_version(repo_root)?;
    let install_root = match (qt_root.filter(|r| !r.is_empty()), system_drive) {
        (Some(root), _) => Some(PathBuf::from(root)),
        (None, Some(drive)) if !drive.is_empty() => {
            let mut root = drive.to_os_string();
            root.push("\\Qt");
            Some(PathBuf::from(root))
        }
        _ => None,
    };
    let root = install_root.map(|r| r.join(&version).join("msvc2022_64"));
    let Some(root) = root.filter(|r| r.is_dir()) else {
        return Ok(QtInstall {
            warning: Some(format!(
                "Qt {version} was not found under the expected install root. Relying on Qt already on PATH."
            )),
            ..QtInstall::default()
        });
    };
    let plugins = root.join("plugins");
    Ok(QtInstall {
        bin: Some(root.join("bin")),
        plugins: plugins.is_dir().then_some(plugins),
        warning: None,
    })
}

/// An ordered list of variables for a child. Later entries win, as they do on
/// a `Command`. Names compare case-insensitively on Windows, where `Path` and
/// `PATH` are one variable.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ChildEnv(pub Vec<(OsString, OsString)>);

impl ChildEnv {
    pub fn set(&mut self, name: impl Into<OsString>, value: impl Into<OsString>) {
        self.0.push((name.into(), value.into()));
    }

    pub fn extend<K: Into<OsString>, V: Into<OsString>>(
        &mut self,
        pairs: impl IntoIterator<Item = (K, V)>,
    ) {
        for (name, value) in pairs {
            self.set(name, value);
        }
    }

    /// The value a child would see: the last entry here, else this process's.
    pub fn get(&self, name: &str) -> Option<OsString> {
        self.0
            .iter()
            .rev()
            .find(|(key, _)| same_name(key, name))
            .map(|(_, value)| value.clone())
            .or_else(|| std::env::var_os(name))
    }

    /// Puts `dir` in front of the PATH a child would otherwise see.
    pub fn prepend_path(&mut self, dir: &Path) {
        let mut value = dir.as_os_str().to_os_string();
        if let Some(rest) = self.get("PATH").filter(|r| !r.is_empty()) {
            value.push(if cfg!(windows) { ";" } else { ":" });
            value.push(rest);
        }
        self.set("PATH", value);
    }

    pub fn apply(&self, command: &mut Command) {
        for (key, value) in &self.0 {
            command.env(key, value);
        }
    }
}

fn same_name(key: &OsStr, name: &str) -> bool {
    let key = key.to_string_lossy();
    if cfg!(windows) {
        key.eq_ignore_ascii_case(name)
    } else {
        key == name
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn repo_with_version(version: Option<&str>) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        if let Some(version) = version {
            std::fs::write(dir.path().join(".qt-version"), format!("{version}\n")).unwrap();
        }
        dir
    }

    #[test]
    fn a_missing_or_malformed_version_is_an_error() {
        let missing = repo_with_version(None);
        assert!(
            canonical_qt_version(missing.path())
                .unwrap_err()
                .to_string()
                .contains("is missing")
        );
        for bad in ["6.11", "6.11.x", "v6.11.2", "6.11.2.1", ""] {
            let repo = repo_with_version(Some(bad));
            let error = canonical_qt_version(repo.path()).unwrap_err().to_string();
            assert!(error.contains("not a three-part version"), "{bad}: {error}");
        }
        let good = repo_with_version(Some("6.11.2"));
        assert_eq!(canonical_qt_version(good.path()).unwrap(), "6.11.2");
    }

    #[test]
    fn the_canonical_install_provides_bin_and_plugins() {
        let repo = repo_with_version(Some("6.11.2"));
        let qt = tempfile::tempdir().unwrap();
        let root = qt.path().join("6.11.2").join("msvc2022_64");
        std::fs::create_dir_all(root.join("bin")).unwrap();
        std::fs::create_dir_all(root.join("plugins")).unwrap();
        let install = resolve_qt(repo.path(), Some(qt.path().as_os_str()), None).unwrap();
        assert_eq!(install.bin, Some(root.join("bin")));
        assert_eq!(install.plugins, Some(root.join("plugins")));
        assert_eq!(install.warning, None);
    }

    #[test]
    fn another_versions_install_is_never_used() {
        let repo = repo_with_version(Some("6.11.2"));
        let qt = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(qt.path().join("6.10.0/msvc2022_64/bin")).unwrap();
        let install = resolve_qt(repo.path(), Some(qt.path().as_os_str()), None).unwrap();
        assert_eq!(install.bin, None);
        assert!(install.warning.unwrap().contains("Qt 6.11.2 was not found"));
    }

    #[test]
    fn without_a_root_or_a_system_drive_the_install_is_absent_not_an_error() {
        let repo = repo_with_version(Some("6.11.2"));
        let install = resolve_qt(repo.path(), None, None).unwrap();
        assert_eq!(install.bin, None);
        assert!(install.warning.is_some());
    }

    #[test]
    fn the_child_path_is_prepended_to_the_one_the_child_would_see() {
        let mut env = ChildEnv::default();
        env.set("PATH", "/base");
        env.prepend_path(Path::new("/qt/bin"));
        let separator = if cfg!(windows) { ";" } else { ":" };
        assert_eq!(
            env.get("PATH").unwrap(),
            OsString::from(format!("/qt/bin{separator}/base"))
        );
    }
}
