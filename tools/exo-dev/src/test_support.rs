//! Shared git-fixture helper for exo-dev's own tests.
//!
//! Reachable from inline `#[cfg(test)]` unit tests in this crate and from
//! integration tests under `tests/` alike, gated behind the `test-support`
//! Cargo feature rather than plain `#[cfg(test)]`: a file under `tests/` is
//! compiled as its own separate crate, so it never sees anything gated purely
//! on `cfg(test)` of the library crate. The feature is real, so it also needs
//! turning on for that separate crate; `Cargo.toml` does this once, with a
//! `[dev-dependencies]` self-reference (`exo-dev = { path = ".", features =
//! ["test-support"] }`) enabling it for every test target without a normal,
//! non-test build ever enabling it.

use std::path::Path;
use std::process::Command;

/// Initializes a throwaway git repository, writes `files` (relative path,
/// content) and stages them. Returns the open `TempDir`; dropping it removes
/// the repository. The tree is left uncommitted on purpose: most callers scope
/// their check to the working tree, which only sees staged/unstaged changes,
/// not a clean initial commit.
pub fn fixture_repo(files: &[(&str, &str)]) -> tempfile::TempDir {
    let dir = tempfile::tempdir().expect("create temp dir for fixture repo");
    run_git(dir.path(), &["init", "-q"]);
    write_files(dir.path(), files);
    run_git(dir.path(), &["add", "-A"]);
    dir
}

/// `fixture_repo`, then commits the staged tree so `HEAD` and merge-base
/// lookups resolve. Needed by anything that scopes to a diff range rather than
/// bare working-tree/staged changes.
pub fn fixture_repo_committed(files: &[(&str, &str)]) -> tempfile::TempDir {
    let dir = fixture_repo(files);
    run_git(
        dir.path(),
        &["config", "user.email", "fixture@example.invalid"],
    );
    run_git(dir.path(), &["config", "user.name", "Fixture"]);
    run_git(dir.path(), &["commit", "-q", "-m", "fixture"]);
    dir
}

/// Writes `files` into an already-initialized repository without staging or
/// committing them, so a test can build on a committed baseline and then
/// exercise the working-tree/staged-changes path.
pub fn write_files(root: &Path, files: &[(&str, &str)]) {
    for (path, content) in files {
        let full = root.join(path);
        std::fs::create_dir_all(full.parent().unwrap()).expect("create fixture parent dir");
        std::fs::write(&full, content).expect("write fixture file");
    }
}

fn run_git(dir: &Path, args: &[&str]) {
    let status = Command::new("git")
        .args(args)
        .current_dir(dir)
        .status()
        .expect("run git for fixture repo");
    assert!(status.success(), "git {args:?} failed in fixture repo");
}
