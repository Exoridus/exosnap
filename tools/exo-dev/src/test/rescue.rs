//! Securing what a suite wrote into its throwaway configuration directory.
//!
//! The QML and cursor-audit suites write their application logs there, and
//! the failure they were run to diagnose is in those logs. The directory is
//! deleted when the run ends, so anything worth keeping is copied out first.

use std::path::Path;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Rescue {
    /// No directory, a passing verdict, or nothing was written.
    NotNeeded,
    /// Copied whole to the destination.
    Rescued { files: usize },
    /// Anything short of a confirmed copy. The original is then the only copy
    /// and the caller must not delete it.
    Failed {
        /// For the receipt's `rescue_detail`.
        detail: String,
        /// Why the evidence could not be secured, for the invalid reason.
        reason: String,
        /// What to tell the reader.
        message: String,
    },
}

/// Copies `config_dir` to `destination` unless `verdict` is a pass. `None`
/// is a run that threw instead of returning a verdict: not evaluated, never a
/// pass, and the run whose logs are worth the most. A directory that cannot
/// be listed is a failure, not an empty directory: treating it as one is how
/// the only copy gets deleted.
pub fn rescue(config_dir: &Path, destination: &Path, verdict: Option<i32>) -> Rescue {
    if !config_dir.exists() || verdict == Some(0) {
        return Rescue::NotNeeded;
    }
    let shown = config_dir.display();
    let left_behind = match count_files(config_dir) {
        Ok(count) => count,
        Err(error) => {
            return Rescue::Failed {
                detail: format!("the source directory could not be listed: {error}"),
                reason: error.to_string(),
                message: format!("Could not list {shown} to secure the test logs: {error}"),
            };
        }
    };
    if left_behind == 0 {
        return Rescue::NotNeeded;
    }
    let copy = || -> Result<usize, String> {
        std::fs::create_dir_all(destination).map_err(|e| e.to_string())?;
        copy_tree(config_dir, destination).map_err(|e| e.to_string())?;
        let copied = count_files(destination).map_err(|e| e.to_string())?;
        if copied < left_behind {
            return Err(format!("copied {copied} of {left_behind} files"));
        }
        Ok(copied)
    };
    match copy() {
        Ok(files) => Rescue::Rescued { files },
        Err(message) => Rescue::Failed {
            detail: message.clone(),
            reason: message.clone(),
            message: format!("Could not secure the test logs from {shown} : {message}"),
        },
    }
}

fn count_files(dir: &Path) -> std::io::Result<usize> {
    let mut count = 0;
    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() {
            count += count_files(&entry.path())?;
        } else {
            count += 1;
        }
    }
    Ok(count)
}

fn copy_tree(from: &Path, to: &Path) -> std::io::Result<()> {
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            std::fs::create_dir_all(&target)?;
            copy_tree(&entry.path(), &target)?;
        } else {
            std::fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_nested_directory_is_copied_whole() {
        let from = tempfile::tempdir().unwrap();
        let to = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(from.path().join("a/b")).unwrap();
        std::fs::write(from.path().join("top.log"), "1").unwrap();
        std::fs::write(from.path().join("a/b/deep.log"), "2").unwrap();
        let destination = to.path().join("run");
        assert_eq!(
            rescue(from.path(), &destination, Some(1)),
            Rescue::Rescued { files: 2 }
        );
        assert_eq!(
            std::fs::read_to_string(destination.join("a/b/deep.log")).unwrap(),
            "2"
        );
    }

    #[test]
    fn a_pass_or_an_empty_directory_needs_no_rescue_and_no_verdict_does() {
        let from = tempfile::tempdir().unwrap();
        let to = tempfile::tempdir().unwrap();
        assert_eq!(
            rescue(from.path(), &to.path().join("x"), None),
            Rescue::NotNeeded
        );
        std::fs::write(from.path().join("app.log"), "1").unwrap();
        assert_eq!(
            rescue(from.path(), &to.path().join("x"), Some(0)),
            Rescue::NotNeeded
        );
        assert_eq!(
            rescue(from.path(), &to.path().join("y"), None),
            Rescue::Rescued { files: 1 }
        );
    }

    #[test]
    fn a_destination_that_cannot_be_created_is_a_failure() {
        let from = tempfile::tempdir().unwrap();
        std::fs::write(from.path().join("app.log"), "1").unwrap();
        let to = tempfile::tempdir().unwrap();
        let occupied = to.path().join("occupied");
        std::fs::write(&occupied, "a file, not a directory").unwrap();
        match rescue(from.path(), &occupied.join("run"), Some(1)) {
            Rescue::Failed { message, .. } => {
                assert!(
                    message.contains("Could not secure the test logs"),
                    "{message}"
                )
            }
            other => panic!("expected a failure, got {other:?}"),
        }
    }
}
