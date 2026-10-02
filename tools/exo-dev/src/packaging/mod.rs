//! Packaging manifest validators: Chocolatey, WinGet, Scoop and the MSI
//! harvest guard.
//!
//! Each validator checks that packaging/<surface>/... agrees with the
//! canonical project version and, for Chocolatey and WinGet, with a handful
//! of regression and moderation rules a regex can decide from the package
//! files alone. A validator returns `Err` when its target manifest cannot
//! even be read (a missing file, an unreadable directory, malformed JSON):
//! that state can never be reported as a passing `ValidationReport`. A
//! version or content disagreement inside an existing, readable manifest is
//! collected into the `ValidationReport` instead, so every disagreement is
//! reported rather than only the first one found.

pub mod chocolatey;
pub mod msi_harvest;
pub mod scoop;
pub mod winget;

pub use chocolatey::{ChocolateyMode, validate_chocolatey};
pub use msi_harvest::validate_msi_harvest;
pub use scoop::validate_scoop;
pub use winget::validate_winget;

use std::path::Path;
use std::sync::LazyLock;

use regex::Regex;

/// The result of one validator run: every disagreement found, plus anything
/// skipped rather than silently passed (Chocolatey's checksum64-vs-manifest
/// check when no release artifact manifest is available).
#[derive(Debug, Default)]
pub struct ValidationReport {
    pub errors: Vec<String>,
    pub skips: Vec<String>,
}

impl ValidationReport {
    pub fn ok(&self) -> bool {
        self.errors.is_empty()
    }
}

/// Renders a validator's skips and errors: every skip first (informational,
/// never a failure), then every error. The caller appends its own final
/// pass/fail line, since each validator's wording (and whether it names a
/// version) differs.
pub fn render(report: &ValidationReport) -> String {
    let mut out = String::new();
    for skip in &report.skips {
        out.push_str(&format!("  [SKIP] {skip}\n"));
    }
    for error in &report.errors {
        out.push_str(&format!("  [FAIL] {error}\n"));
    }
    out
}

/// Parses `project(exosnap VERSION x.y.z ...)` from the root CMakeLists.txt,
/// the canonical version every packaging validator defaults to.
pub fn cmake_project_version(repo_root: &Path) -> anyhow::Result<String> {
    static PATTERN: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"project\(\s*exosnap\s+VERSION\s+(\d+\.\d+\.\d+)").unwrap());
    let path = repo_root.join("CMakeLists.txt");
    let text = std::fs::read_to_string(&path)
        .map_err(|error| anyhow::anyhow!("could not read {}: {error}", path.display()))?;
    PATTERN
        .captures(&text)
        .map(|c| c[1].to_string())
        .ok_or_else(|| {
            anyhow::anyhow!(
                "could not parse project(exosnap VERSION x.y.z) from {}",
                path.display()
            )
        })
}

/// Every `\d+.\d+.\d+`-shaped substring in `line` that is not part of a
/// longer dotted-number run, with its byte offset. A version-looking token
/// counts only when the character immediately before and after it (if any)
/// is neither a digit nor a `.`, which excludes a substring of a longer
/// dotted-number run such as an MSVC toolset version. Rust's `regex` crate
/// has no lookaround, so this boundary check is done manually on the raw
/// bytes around each match instead of in the pattern itself.
pub(crate) fn bare_version_matches(line: &str) -> Vec<(usize, String)> {
    static VERSION_LIKE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\d+\.\d+\.\d+").unwrap());
    let bytes = line.as_bytes();
    VERSION_LIKE
        .find_iter(line)
        .filter(|m| {
            let before_ok = m.start() == 0 || !matches!(bytes[m.start() - 1], b'0'..=b'9' | b'.');
            let after_ok = m.end() == bytes.len() || !matches!(bytes[m.end()], b'0'..=b'9' | b'.');
            before_ok && after_ok
        })
        .map(|m| (m.start(), m.as_str().to_string()))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cmake_project_version_reads_the_canonical_declaration() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join("CMakeLists.txt"),
            "cmake_minimum_required(VERSION 3.25)\nproject(exosnap VERSION 0.10.0 LANGUAGES C CXX)\n",
        )
        .unwrap();
        assert_eq!(cmake_project_version(dir.path()).unwrap(), "0.10.0");
    }

    #[test]
    fn cmake_project_version_is_a_hard_error_when_unparseable() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("CMakeLists.txt"), "project(other)\n").unwrap();
        assert!(cmake_project_version(dir.path()).is_err());
    }

    #[test]
    fn bare_version_matches_excludes_longer_dotted_runs() {
        assert_eq!(
            bare_version_matches("plain 0.10.0 here"),
            vec![(6, "0.10.0".to_string())]
        );
        assert!(bare_version_matches("vcredist140 version 14.44.35112.1").is_empty());
    }

    #[test]
    fn bare_version_matches_finds_more_than_one_per_line() {
        let found = bare_version_matches("0.9.0 then 0.10.0");
        assert_eq!(found.len(), 2);
        assert_eq!(found[0].1, "0.9.0");
        assert_eq!(found[1].1, "0.10.0");
    }
}
