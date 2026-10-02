//! The identity of the source a build or a test result is about.
//!
//! A test result is only evidence if it can name what it was produced from.
//! This is that name: a content digest of HEAD, of every tracked
//! modification and of the CONTENT of every untracked, non-ignored file in
//! the working tree.
//!
//! Timestamps are not usable for the question. A restored or copied tree
//! keeps the mtimes it was created with, so a stale tree can look newer
//! than the source it is missing.
//!
//! Two equal digests, taken at two moments, do not prove the tree was
//! untouched in between: an edit that was made and reverted leaves no trace
//! here. Combined with a lock that keeps other cooperating entry points out,
//! they do prove that the source a build was made from is the source its
//! result is attributed to.

use std::path::Path;

use crate::git::Git;

/// Above these limits a tree is not treated as a source tree, and no digest
/// is produced. Generated output left under the root is the usual cause,
/// and hashing it would make the digest a statement about build products.
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub max_untracked_files: usize,
    pub max_untracked_bytes: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            max_untracked_files: 4000,
            max_untracked_bytes: 256 * 1024 * 1024,
        }
    }
}

/// The identity of a working tree, or a stated reason none could be
/// produced. Every failure carries a reason rather than an empty digest,
/// which would compare equal to every other failure.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SourceIdentity {
    Known {
        head: String,
        dirty: bool,
        fingerprint: String,
        untracked_files: usize,
        untracked_bytes: u64,
    },
    Unavailable {
        reason: String,
    },
}

impl SourceIdentity {
    /// The digest, when one could be produced.
    pub fn fingerprint(&self) -> Option<&str> {
        match self {
            SourceIdentity::Known { fingerprint, .. } => Some(fingerprint),
            SourceIdentity::Unavailable { .. } => None,
        }
    }

    pub fn is_known(&self) -> bool {
        self.fingerprint().is_some()
    }
}

/// The identity of `repo_root`'s working tree, under the module's default
/// caps. See `identify_within` for injectable caps.
pub fn identify(repo_root: &Path) -> SourceIdentity {
    identify_within(repo_root, &Limits::default())
}

/// The digest covers HEAD, `git diff HEAD` and the content of every file
/// `git ls-files --others --exclude-standard` reports. Untracked files are
/// hashed, not merely listed: a new source file the build compiles is
/// untracked until it is added, so a listing by name would report the same
/// digest before and after every edit to it.
pub fn identify_within(repo_root: &Path, limits: &Limits) -> SourceIdentity {
    let git = Git::new(repo_root);

    let (code, head_out) = git.run(&["rev-parse", "HEAD"]);
    let head = head_out.trim().to_string();
    if code != 0 || head.is_empty() {
        return unavailable("git rev-parse HEAD failed");
    }

    let (code, diff) = git.run(&["diff", "HEAD"]);
    if code != 0 {
        return unavailable("git diff HEAD failed");
    }

    let (code, untracked_out) = git.run(&["ls-files", "--others", "--exclude-standard"]);
    if code != 0 {
        return unavailable("git ls-files --others failed");
    }
    let mut untracked: Vec<String> = untracked_out
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(str::to_string)
        .collect();
    untracked.sort();

    if untracked.len() > limits.max_untracked_files {
        return unavailable(format!(
            "{} untracked files exceed the {} this run will hash",
            untracked.len(),
            limits.max_untracked_files
        ));
    }

    let mut parts: Vec<String> = Vec::with_capacity(2 + untracked.len() * 2);
    parts.push(head.clone());
    parts.push(diff.clone());

    let mut untracked_bytes: u64 = 0;
    for relative in &untracked {
        let full = repo_root.join(relative);
        let metadata = match std::fs::metadata(&full) {
            Ok(metadata) => metadata,
            Err(error) => {
                return unavailable(format!(
                    "untracked file '{relative}' could not be read: {error}"
                ));
            }
        };
        // git never lists a directory itself from `ls-files --others`, but a
        // reparse point resolving to one is possible. It is skipped rather
        // than hashed as if it were file content.
        if metadata.is_dir() {
            continue;
        }
        untracked_bytes += metadata.len();
        if untracked_bytes > limits.max_untracked_bytes {
            return unavailable(format!(
                "untracked files exceed {} MB; not hashed",
                limits.max_untracked_bytes / (1024 * 1024)
            ));
        }
        let content = match std::fs::read(&full) {
            Ok(content) => content,
            Err(error) => {
                return unavailable(format!(
                    "untracked file '{relative}' could not be read: {error}"
                ));
            }
        };
        parts.push(relative.clone());
        parts.push(hex::encode_upper(sha256(&content)));
    }

    let dirty = !diff.trim().is_empty() || !untracked.is_empty();
    let fingerprint = hex::encode(sha256(parts.join("\n").as_bytes()));

    SourceIdentity::Known {
        head,
        dirty,
        fingerprint,
        untracked_files: untracked.len(),
        untracked_bytes,
    }
}

fn unavailable(reason: impl Into<String>) -> SourceIdentity {
    SourceIdentity::Unavailable {
        reason: reason.into(),
    }
}

fn sha256(bytes: &[u8]) -> impl AsRef<[u8]> {
    use sha2::{Digest, Sha256};
    Sha256::digest(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::{fixture_repo_committed, write_files};

    fn cap(max_untracked_files: usize, max_untracked_bytes: u64) -> Limits {
        Limits {
            max_untracked_files,
            max_untracked_bytes,
        }
    }

    #[test]
    fn the_same_tree_twice_produces_the_same_digest() {
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        let first = identify(repo.path());
        let second = identify(repo.path());
        assert_eq!(first.fingerprint(), second.fingerprint());
        match first {
            SourceIdentity::Known { dirty, .. } => {
                assert!(!dirty, "a clean tree reported itself dirty")
            }
            SourceIdentity::Unavailable { reason } => panic!("no digest: {reason}"),
        }
    }

    #[test]
    fn a_modification_to_a_tracked_file_changes_the_digest() {
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        let before = identify(repo.path());
        write_files(repo.path(), &[("tracked.txt", "two\n")]);
        let after = identify(repo.path());
        assert_ne!(
            before.fingerprint(),
            after.fingerprint(),
            "an edited tracked file left the digest unchanged"
        );
        match after {
            SourceIdentity::Known { dirty, .. } => {
                assert!(dirty, "a modified tree did not report itself dirty")
            }
            SourceIdentity::Unavailable { reason } => panic!("no digest: {reason}"),
        }
    }

    #[test]
    fn the_content_of_an_untracked_file_is_part_of_the_digest() {
        // The hole this closes. A new source file is untracked until it is
        // added, and a digest over untracked names reports the same value
        // before and after every edit to it, so a test result would be
        // attributed to source the build never saw.
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        write_files(
            repo.path(),
            &[("new-source.cpp", "int main() { return 0; }\n")],
        );
        let before = identify(repo.path());
        let SourceIdentity::Known {
            untracked_files, ..
        } = &before
        else {
            panic!("expected a digest");
        };
        assert_eq!(*untracked_files, 1);

        write_files(
            repo.path(),
            &[("new-source.cpp", "int main() { return 1; }\n")],
        );
        let after = identify(repo.path());
        assert_ne!(
            before.fingerprint(),
            after.fingerprint(),
            "an untracked file was edited and the digest did not move"
        );
        match after {
            SourceIdentity::Known { dirty, .. } => {
                assert!(
                    dirty,
                    "a tree with an untracked file did not report itself dirty"
                )
            }
            SourceIdentity::Unavailable { reason } => panic!("no digest: {reason}"),
        }
    }

    #[test]
    fn an_ignored_file_is_not_part_of_the_digest() {
        // Build output lives under the working tree. Hashing it would turn
        // the source identity into a statement about build products, and
        // every build would look like a source change.
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n"), (".gitignore", "build/\n")]);
        std::fs::create_dir_all(repo.path().join("build")).unwrap();
        let before = identify(repo.path());
        write_files(repo.path(), &[("build/artifact.bin", "output\n")]);
        let after = identify(repo.path());
        assert_eq!(
            before.fingerprint(),
            after.fingerprint(),
            "ignored build output moved the source digest"
        );
    }

    #[test]
    fn two_untracked_files_cannot_be_swapped_without_moving_the_digest() {
        // A digest built from a set of content hashes with no names in it
        // would be identical for two files whose contents were exchanged.
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        write_files(repo.path(), &[("a.txt", "alpha"), ("b.txt", "beta")]);
        let before = identify(repo.path());
        write_files(repo.path(), &[("a.txt", "beta"), ("b.txt", "alpha")]);
        let after = identify(repo.path());
        assert_ne!(
            before.fingerprint(),
            after.fingerprint(),
            "two untracked files exchanged their contents and the digest did not move"
        );
    }

    #[test]
    fn a_directory_that_is_not_a_repository_yields_a_stated_failure_not_a_digest() {
        // An empty digest would compare equal to the next failure, and two
        // runs that both failed to identify their source would look like two
        // runs of the same one.
        let dir = tempfile::tempdir().unwrap();
        let result = identify(dir.path());
        assert!(
            !result.is_known(),
            "a directory outside any repository produced a digest"
        );
        match result {
            SourceIdentity::Unavailable { reason } => {
                assert!(
                    reason.contains("rev-parse"),
                    "the failure did not say what failed: {reason}"
                );
            }
            SourceIdentity::Known { .. } => unreachable!(),
        }
    }

    #[test]
    fn a_tree_with_more_untracked_files_than_the_cap_is_refused_not_summarised() {
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        for n in 1..=5 {
            write_files(repo.path(), &[(&format!("extra-{n}.txt"), &n.to_string())]);
        }
        let result = identify_within(repo.path(), &cap(3, Limits::default().max_untracked_bytes));
        assert!(
            !result.is_known(),
            "a tree above the untracked-file cap produced a digest anyway"
        );
        match result {
            SourceIdentity::Unavailable { reason } => assert!(
                reason.contains("untracked files exceed"),
                "unexpected reason: {reason}"
            ),
            SourceIdentity::Known { .. } => unreachable!(),
        }
    }

    #[test]
    fn untracked_content_above_the_byte_cap_is_refused() {
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        write_files(repo.path(), &[("big.bin", &"x".repeat(4096))]);
        let result = identify_within(
            repo.path(),
            &cap(Limits::default().max_untracked_files, 1024),
        );
        assert!(
            !result.is_known(),
            "a tree above the untracked-byte cap produced a digest anyway"
        );
        match result {
            SourceIdentity::Unavailable { reason } => {
                assert!(reason.contains("exceed"), "unexpected reason: {reason}")
            }
            SourceIdentity::Known { .. } => unreachable!(),
        }
    }

    #[cfg(unix)]
    fn create_unreadable_placeholder(path: &Path) {
        std::os::unix::fs::symlink("/exosnap-fingerprint-test-missing-target", path).unwrap();
    }

    #[cfg(windows)]
    fn create_unreadable_placeholder(path: &Path) {
        std::os::windows::fs::symlink_file("C:\\exosnap-fingerprint-test-missing-target", path)
            .unwrap();
    }

    #[test]
    fn an_untracked_file_git_lists_and_this_code_cannot_read_is_a_failure() {
        // An unreadable input is an unknown input. Skipping it would produce
        // a digest that silently omits part of the source. A dangling
        // symlink is a portable way to make git list a path this code then
        // fails to read, without depending on OS-enforced file locking.
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        let locked = repo.path().join("locked.txt");
        create_unreadable_placeholder(&locked);
        let result = identify(repo.path());
        assert!(
            !result.is_known(),
            "an unreadable untracked file was silently left out of the digest"
        );
        match result {
            SourceIdentity::Unavailable { reason } => assert!(
                reason.contains("locked.txt"),
                "the failure did not name the file: {reason}"
            ),
            SourceIdentity::Known { .. } => unreachable!(),
        }
    }

    #[test]
    fn an_edit_that_was_reverted_is_indistinguishable_and_the_docs_say_so() {
        // Not a defect to fix here: a content digest cannot see history. It
        // is stated so that nobody reads the before/after pair as proof of
        // an untouched tree. The host tree lock is what keeps other entry
        // points out, and this pair is what ties the result to a source.
        let repo = fixture_repo_committed(&[("tracked.txt", "one\n")]);
        let before = identify(repo.path());
        write_files(repo.path(), &[("tracked.txt", "two\n")]);
        write_files(repo.path(), &[("tracked.txt", "one\n")]);
        let after = identify(repo.path());
        assert_eq!(
            before.fingerprint(),
            after.fingerprint(),
            "the premise of the documented limitation no longer holds; the documentation has to change with it"
        );
    }
}
