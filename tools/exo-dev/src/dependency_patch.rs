//! Idempotent application of pinned patches to downloaded dependency sources.

use std::path::Path;

use anyhow::{Context, Result, bail};
use sha2::{Digest, Sha256};

#[derive(Debug, PartialEq, Eq)]
pub enum PatchOutcome {
    Applied,
    AlreadyApplied,
}

/// Applies the complete patch or verifies it is already present. Conflicting
/// source changes are preserved and reported as errors.
pub fn apply(source: &Path, patch: &Path, expected_sha256: Option<&str>) -> Result<PatchOutcome> {
    let patch = std::path::absolute(patch).context("resolve dependency patch")?;
    if let Some(expected) = expected_sha256 {
        let actual = hex::encode(Sha256::digest(std::fs::read(&patch)?));
        if !actual.eq_ignore_ascii_case(expected) {
            bail!("dependency patch digest mismatch: expected {expected}, got {actual}");
        }
    }
    let run = |args: &[&str]| -> Result<std::process::Output> {
        crate::process::command("git")
            .arg("-C")
            .arg(source)
            .arg("apply")
            .args(args)
            .arg("--")
            .arg(&patch)
            .output()
            .context("run git apply for dependency patch")
    };
    let applicable = run(&["--check"])?;
    if applicable.status.success() {
        let applied = run(&[])?;
        if !applied.status.success() {
            bail!(
                "dependency patch application failed: {}",
                String::from_utf8_lossy(&applied.stderr)
            );
        }
        return Ok(PatchOutcome::Applied);
    }
    let present = run(&["--reverse", "--check"])?;
    if present.status.success() {
        return Ok(PatchOutcome::AlreadyApplied);
    }
    bail!(
        "dependency patch is neither applicable nor already applied in {}: {}",
        source.display(),
        String::from_utf8_lossy(&applicable.stderr)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    const PATCH: &str = "diff --git a/value.txt b/value.txt\n--- a/value.txt\n+++ b/value.txt\n@@ -1 +1 @@\n-before\n+after\n";

    fn fixture() -> (tempfile::TempDir, tempfile::TempDir) {
        let source = crate::test_support::fixture_repo_committed(&[("value.txt", "before\n")]);
        let patches = tempfile::tempdir().unwrap();
        std::fs::write(patches.path().join("change.patch"), PATCH).unwrap();
        (source, patches)
    }

    #[test]
    fn applies_fresh_patch_and_accepts_repeated_configure() {
        let (source, patches) = fixture();
        let patch = patches.path().join("change.patch");
        assert_eq!(
            apply(source.path(), &patch, None).unwrap(),
            PatchOutcome::Applied
        );
        assert_eq!(
            std::fs::read_to_string(source.path().join("value.txt")).unwrap(),
            "after\n"
        );
        assert_eq!(
            apply(source.path(), &patch, None).unwrap(),
            PatchOutcome::AlreadyApplied
        );
    }

    #[test]
    fn refuses_conflict_without_changing_the_source() {
        let (source, patches) = fixture();
        std::fs::write(source.path().join("value.txt"), "local change\n").unwrap();
        let error = apply(source.path(), &patches.path().join("change.patch"), None).unwrap_err();
        assert!(
            error
                .to_string()
                .contains("neither applicable nor already applied")
        );
        assert_eq!(
            std::fs::read_to_string(source.path().join("value.txt")).unwrap(),
            "local change\n"
        );
    }

    #[test]
    fn refuses_changed_patch_bytes_before_applying() {
        let (source, patches) = fixture();
        let error = apply(
            source.path(),
            &patches.path().join("change.patch"),
            Some(&"0".repeat(64)),
        )
        .unwrap_err();
        assert!(error.to_string().contains("digest mismatch"));
        assert_eq!(
            std::fs::read_to_string(source.path().join("value.txt")).unwrap(),
            "before\n"
        );
    }
}
