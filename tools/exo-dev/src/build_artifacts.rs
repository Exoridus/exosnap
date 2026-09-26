//! Locating a binary this repository builds, across the build trees that exist.
//!
//! Four trees can hold the same tool: a Ninja and a Visual Studio tree, each in
//! a Debug and a Release configuration. They are built at different times and
//! none of them is authoritative, so "the first match" resolves to whichever
//! tree happens to be listed first, not to the freshest build.
//!
//! For an instrument, such as a probe binary, the newest build is the right
//! answer: [`resolve_built_artifact`] returns it. For the product binary under
//! test the choice between Debug and Release is deliberate, so that caller
//! keeps its own order and calls [`newer_artifact_than`] to learn when it
//! passed over something newer, rather than have this module override it.
//!
//! The two tree layouts differ: MSBuild puts the configuration in a directory
//! of its own (`tools/envctl/Debug/exosnap-envctl.exe`), Ninja does not
//! (`tools/envctl/exosnap-envctl.exe`). Callers name the Ninja shape. Both
//! shapes are searched.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use anyhow::{Context, Result};

/// One directory a local build can land in, with the configuration directory
/// MSBuild inserts (a Ninja tree inserts none). Order carries no meaning here.
/// Selection is by build time.
struct Tree {
    directory: &'static str,
    configurations: &'static [&'static str],
}

const TREES: [Tree; 4] = [
    Tree {
        directory: "build/windows-x64-ninja-release",
        configurations: &[],
    },
    Tree {
        directory: "build/windows-x64-ninja-debug",
        configurations: &[],
    },
    Tree {
        directory: "build/windows-x64-release",
        configurations: &["Release"],
    },
    Tree {
        directory: "build/windows-x64-debug",
        configurations: &["Debug"],
    },
];

/// A build of an artifact found in one of the local build trees.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuiltArtifact {
    pub path: PathBuf,
    /// The tree directory holding it, relative to the repository root, for
    /// example `build/windows-x64-ninja-debug`.
    pub tree: String,
    pub built_at: SystemTime,
}

/// Every path a tree could hold this artifact at, whether or not it exists,
/// paired with the tree directory it belongs to.
fn candidates(repo_root: &Path, relative_path: &Path) -> Vec<(PathBuf, &'static str)> {
    let directory = relative_path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty());
    let leaf = relative_path
        .file_name()
        .expect("relative_path names a file");

    let mut out = Vec::with_capacity(TREES.len() * 2);
    for tree in &TREES {
        let tree_root = repo_root.join(tree.directory);
        out.push((tree_root.join(relative_path), tree.directory));
        for configuration in tree.configurations {
            let with_configuration = match directory {
                Some(dir) => dir.join(configuration).join(leaf),
                None => Path::new(configuration).join(leaf),
            };
            out.push((tree_root.join(with_configuration), tree.directory));
        }
    }
    out
}

/// The most recently built copy of `relative_path` across all four build
/// trees, or `Ok(None)` when no tree holds one.
///
/// `relative_path` names the artifact in the Ninja layout, for example
/// `tools/envctl/exosnap-envctl.exe`. The equivalent MSBuild configuration
/// directory is searched too. A machine that never built the tool can still
/// run everything that does not need it, so a missing artifact is `Ok(None)`,
/// never an error: a caller reports UNAVAILABLE with a reason, stating
/// something true about that run, rather than failing outright.
pub fn resolve_built_artifact(
    repo_root: &Path,
    relative_path: &Path,
) -> Result<Option<BuiltArtifact>> {
    let mut newest: Option<BuiltArtifact> = None;
    for (candidate, tree) in candidates(repo_root, relative_path) {
        let metadata = match fs::metadata(&candidate) {
            Ok(metadata) if metadata.is_file() => metadata,
            Ok(_) => continue,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
            Err(error) => {
                return Err(error)
                    .with_context(|| format!("reading metadata for {}", candidate.display()));
            }
        };
        let built_at = metadata
            .modified()
            .with_context(|| format!("reading modification time for {}", candidate.display()))?;
        if newest
            .as_ref()
            .is_none_or(|current| built_at > current.built_at)
        {
            newest = Some(BuiltArtifact {
                path: candidate,
                tree: tree.to_string(),
                built_at,
            });
        }
    }
    Ok(newest)
}

/// Whether some other tree holds a strictly newer build of `relative_path`
/// than the caller's already-chosen `chosen` path.
///
/// For the binary under test, preferring Release over Debug (or vice versa)
/// is a decision, not an accident, so this reports rather than overrides it.
/// What it catches is the silent case: a campaign verifying a Release build
/// from last week while the change it is meant to cover sits unbuilt, newer,
/// in another tree.
///
/// Returns `Ok(None)` both when `chosen` is already the newest build and when
/// no tree holds anything newer. Path comparison against `chosen` is
/// case-insensitive, matching Windows path semantics.
pub fn newer_artifact_than(
    repo_root: &Path,
    chosen: &Path,
    relative_path: &Path,
) -> Result<Option<BuiltArtifact>> {
    let Some(newest) = resolve_built_artifact(repo_root, relative_path)? else {
        return Ok(None);
    };

    let chosen_canonical = fs::canonicalize(chosen)
        .with_context(|| format!("resolving chosen path {}", chosen.display()))?;
    let newest_canonical = fs::canonicalize(&newest.path)
        .with_context(|| format!("resolving resolved artifact {}", newest.path.display()))?;
    if paths_equal_ignore_case(&chosen_canonical, &newest_canonical) {
        return Ok(None);
    }

    let chosen_at = fs::metadata(&chosen_canonical)
        .with_context(|| format!("reading metadata for {}", chosen_canonical.display()))?
        .modified()
        .with_context(|| {
            format!(
                "reading modification time for {}",
                chosen_canonical.display()
            )
        })?;
    if newest.built_at <= chosen_at {
        return Ok(None);
    }
    Ok(Some(newest))
}

fn paths_equal_ignore_case(a: &Path, b: &Path) -> bool {
    a.to_string_lossy().to_lowercase() == b.to_string_lossy().to_lowercase()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn set_built_at(path: &Path, offset_seconds: u64) {
        let time = SystemTime::UNIX_EPOCH + Duration::from_secs(offset_seconds);
        std::fs::File::options()
            .write(true)
            .open(path)
            .expect("open fixture artifact to set its modification time")
            .set_modified(time)
            .expect("set fixture artifact modification time");
    }

    fn write_artifact(root: &Path, relative: &str, offset_seconds: u64) -> PathBuf {
        let full = root.join(relative);
        std::fs::create_dir_all(full.parent().expect("fixture path has a parent"))
            .expect("create fixture artifact directory");
        std::fs::write(&full, "binary").expect("write fixture artifact");
        set_built_at(&full, offset_seconds);
        full
    }

    // The shipped defect: the release tree was listed first and answered every
    // call, so a campaign ran a week-old tool while the afternoon's build sat
    // unused in another tree.
    #[test]
    fn the_newest_build_wins_over_the_one_that_comes_first_alphabetically() {
        let root = tempfile::tempdir().unwrap();
        write_artifact(
            root.path(),
            "build/windows-x64-release/tools/envctl/Release/exosnap-envctl.exe",
            1_000,
        );
        let fresh = write_artifact(
            root.path(),
            "build/windows-x64-ninja-debug/tools/envctl/exosnap-envctl.exe",
            2_000,
        );

        let resolved =
            resolve_built_artifact(root.path(), Path::new("tools/envctl/exosnap-envctl.exe"))
                .unwrap()
                .expect("an artifact was found");

        assert_eq!(resolved.path, fresh);
        assert_eq!(resolved.tree, "build/windows-x64-ninja-debug");
    }

    #[test]
    fn the_msbuild_configuration_directory_is_searched_as_well_as_the_ninja_shape() {
        let root = tempfile::tempdir().unwrap();
        let only = write_artifact(
            root.path(),
            "build/windows-x64-debug/tools/envctl/Debug/exosnap-envctl.exe",
            500,
        );

        let resolved =
            resolve_built_artifact(root.path(), Path::new("tools/envctl/exosnap-envctl.exe"))
                .unwrap()
                .expect("the MSBuild layout was not found at all");

        assert_eq!(resolved.path, only);
        assert_eq!(resolved.tree, "build/windows-x64-debug");
    }

    #[test]
    fn a_tree_that_holds_no_copy_resolves_to_nothing_instead_of_erroring() {
        let root = tempfile::tempdir().unwrap();

        let resolved =
            resolve_built_artifact(root.path(), Path::new("tools/envctl/exosnap-envctl.exe"))
                .unwrap();

        assert!(resolved.is_none());
    }

    #[test]
    fn a_newer_build_elsewhere_is_reported_to_a_caller_that_keeps_its_own_order() {
        let root = tempfile::tempdir().unwrap();
        let chosen = write_artifact(
            root.path(),
            "build/windows-x64-release/app/Release/exosnap.exe",
            1_000,
        );
        write_artifact(
            root.path(),
            "build/windows-x64-ninja-debug/app/exosnap.exe",
            2_000,
        );

        let newer = newer_artifact_than(root.path(), &chosen, Path::new("app/exosnap.exe"))
            .unwrap()
            .expect("a build six days newer was not reported");

        assert_eq!(newer.tree, "build/windows-x64-ninja-debug");
    }

    #[test]
    fn the_newest_build_being_the_chosen_one_is_reported_as_nothing_to_say() {
        let root = tempfile::tempdir().unwrap();
        let chosen = write_artifact(
            root.path(),
            "build/windows-x64-ninja-release/app/exosnap.exe",
            2_000,
        );
        write_artifact(
            root.path(),
            "build/windows-x64-debug/app/Debug/exosnap.exe",
            1_000,
        );

        let newer =
            newer_artifact_than(root.path(), &chosen, Path::new("app/exosnap.exe")).unwrap();

        assert!(newer.is_none());
    }

    #[test]
    fn path_equality_against_chosen_is_case_insensitive() {
        let root = tempfile::tempdir().unwrap();
        let chosen = write_artifact(
            root.path(),
            "build/windows-x64-debug/app/Debug/exosnap.exe",
            1_000,
        );
        let chosen_upper = PathBuf::from(chosen.to_string_lossy().to_uppercase());

        assert!(paths_equal_ignore_case(&chosen, &chosen_upper));
        assert!(!paths_equal_ignore_case(&chosen, Path::new("/other/path")));
    }
}
