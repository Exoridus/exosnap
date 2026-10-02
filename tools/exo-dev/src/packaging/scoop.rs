//! Scoop manifest validator: packaging/scoop/exosnap.json against the target
//! ExoSnap version.
//!
//! Checked: `version` equals the target version; `architecture.64bit.url` is
//! the GitHub Release portable ZIP for that version (tag segment and file
//! name both); `architecture.64bit.hash` is 64 lowercase hex characters, or
//! the placeholder a bumped-but-unpublished release legitimately carries (the
//! installer hash is not cross-checked against a published release: that
//! needs the network, and this runs on every pull request);
//! `architecture.64bit.extract_dir` is the directory that ZIP unpacks to; no
//! other `X.Y.Z`-shaped literal in the file names a different version; the
//! `autoupdate` block still templates on `$version` rather than hard-coding
//! one, which is what makes the published bucket entry self-updating at all.

use std::path::Path;
use std::sync::LazyLock;

use regex::Regex;
use serde_json::Value;

use super::{ValidationReport, bare_version_matches};

pub fn validate_scoop(repo_root: &Path, version: &str) -> anyhow::Result<ValidationReport> {
    let manifest_path = repo_root.join("packaging/scoop/exosnap.json");
    if !manifest_path.is_file() {
        anyhow::bail!("Scoop manifest not found: {}", manifest_path.display());
    }

    let text = std::fs::read_to_string(&manifest_path)?;
    let manifest: Value = serde_json::from_str(&text).map_err(|error| {
        anyhow::anyhow!("{} is not valid JSON: {error}", manifest_path.display())
    })?;

    let mut report = ValidationReport::default();

    let manifest_version = manifest
        .get("version")
        .and_then(Value::as_str)
        .unwrap_or_default();
    if manifest_version != version {
        report.errors.push(format!(
            "version '{manifest_version}' != target version '{version}'"
        ));
    }

    let architecture = manifest.get("architecture").and_then(|a| a.get("64bit"));
    let url = architecture
        .and_then(|a| a.get("url"))
        .and_then(Value::as_str)
        .unwrap_or_default();
    let expected_url = format!(
        "https://github.com/Exoridus/exosnap/releases/download/v{version}/ExoSnap-{version}-windows-x64-portable.zip"
    );
    if url != expected_url {
        report.errors.push(format!(
            "architecture.64bit.url '{url}' != expected '{expected_url}'"
        ));
    }

    let extract_dir = architecture
        .and_then(|a| a.get("extract_dir"))
        .and_then(Value::as_str)
        .unwrap_or_default();
    let expected_extract_dir = format!("ExoSnap-{version}-windows-x64-portable");
    if extract_dir != expected_extract_dir {
        report.errors.push(format!(
            "architecture.64bit.extract_dir '{extract_dir}' != expected '{expected_extract_dir}'"
        ));
    }

    let hash = architecture
        .and_then(|a| a.get("hash"))
        .and_then(Value::as_str)
        .unwrap_or_default();
    static HEX64: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[0-9a-f]{64}$").unwrap());
    if !HEX64.is_match(hash) {
        report.errors.push(format!(
            "architecture.64bit.hash '{hash}' is not 64 lowercase hex characters"
        ));
    }

    let autoupdate = manifest
        .get("autoupdate")
        .and_then(|a| a.get("architecture"))
        .and_then(|a| a.get("64bit"));
    for field in ["url", "extract_dir"] {
        let value = autoupdate
            .and_then(|a| a.get(field))
            .and_then(Value::as_str)
            .unwrap_or_default();
        if !value.contains("$version") {
            report.errors.push(format!(
                "autoupdate.architecture.64bit.{field} does not template on $version: '{value}'"
            ));
        }
    }
    let hash_url = autoupdate
        .and_then(|a| a.get("hash"))
        .and_then(|h| h.get("url"))
        .and_then(Value::as_str)
        .unwrap_or_default();
    if !hash_url.contains("$version") {
        report.errors.push(format!(
            "autoupdate.architecture.64bit.hash.url does not template on $version: '{hash_url}'"
        ));
    }

    for (idx, line) in text.lines().enumerate() {
        if line.contains("$version") {
            continue;
        }
        for (_, value) in bare_version_matches(line) {
            if value != version {
                report.errors.push(format!(
                    "line {}: stale version reference '{value}' in: {}",
                    idx + 1,
                    line.trim()
                ));
            }
        }
    }

    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    const VERSION: &str = "0.10.0";

    const MANIFEST: &str = r#"{
    "version": "0.10.0",
    "description": "Windows-native screen recorder.",
    "homepage": "https://github.com/Exoridus/exosnap",
    "license": "GPL-3.0-or-later",
    "depends": "extras/vcredist2022",
    "notes": [
        "ExoSnap 0.10.0 is a pre-v1 preview."
    ],
    "architecture": {
        "64bit": {
            "url": "https://github.com/Exoridus/exosnap/releases/download/v0.10.0/ExoSnap-0.10.0-windows-x64-portable.zip",
            "hash": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "extract_dir": "ExoSnap-0.10.0-windows-x64-portable"
        }
    },
    "shortcuts": [
        ["exosnap.exe", "ExoSnap"]
    ],
    "checkver": {
        "github": "https://github.com/Exoridus/exosnap"
    },
    "autoupdate": {
        "architecture": {
            "64bit": {
                "url": "https://github.com/Exoridus/exosnap/releases/download/v$version/ExoSnap-$version-windows-x64-portable.zip",
                "hash": {
                    "url": "https://github.com/Exoridus/exosnap/releases/download/v$version/ExoSnap-$version-windows-x64-portable.sha256"
                },
                "extract_dir": "ExoSnap-$version-windows-x64-portable"
            }
        }
    }
}
"#;

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("packaging/scoop")).unwrap();
        std::fs::write(dir.path().join("packaging/scoop/exosnap.json"), MANIFEST).unwrap();
        dir
    }

    #[test]
    fn a_consistent_manifest_passes() {
        let dir = fixture();
        let report = validate_scoop(dir.path(), VERSION).unwrap();
        assert!(report.ok(), "{:?}", report.errors);
    }

    #[test]
    fn a_missing_manifest_is_a_hard_error() {
        let dir = tempfile::tempdir().unwrap();
        let error = validate_scoop(dir.path(), VERSION).unwrap_err();
        assert!(error.to_string().contains("Scoop manifest not found"));
    }

    #[test]
    fn invalid_json_is_a_hard_error() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/scoop/exosnap.json"),
            "{ not json",
        )
        .unwrap();
        let error = validate_scoop(dir.path(), VERSION).unwrap_err();
        assert!(error.to_string().contains("not valid JSON"));
    }

    #[test]
    fn a_version_field_disagreeing_with_the_target_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/scoop/exosnap.json"),
            MANIFEST.replacen("\"version\": \"0.10.0\"", "\"version\": \"0.9.0\"", 1),
        )
        .unwrap();
        let report = validate_scoop(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("version '0.9.0' != target version"))
        );
    }

    #[test]
    fn a_hash_that_is_not_lowercase_hex_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/scoop/exosnap.json"),
            MANIFEST.replace(
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                "NOTAHASH",
            ),
        )
        .unwrap();
        let report = validate_scoop(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("is not 64 lowercase hex characters"))
        );
    }

    #[test]
    fn an_autoupdate_url_hard_coding_the_version_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/scoop/exosnap.json"),
            MANIFEST.replace(
                "\"url\": \"https://github.com/Exoridus/exosnap/releases/download/v$version/ExoSnap-$version-windows-x64-portable.zip\"",
                "\"url\": \"https://github.com/Exoridus/exosnap/releases/download/v0.10.0/ExoSnap-0.10.0-windows-x64-portable.zip\"",
            ),
        )
        .unwrap();
        let report = validate_scoop(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("does not template on $version"))
        );
    }

    #[test]
    fn a_stale_version_reference_in_notes_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/scoop/exosnap.json"),
            MANIFEST.replace(
                "ExoSnap 0.10.0 is a pre-v1 preview.",
                "ExoSnap 0.9.0 is a pre-v1 preview.",
            ),
        )
        .unwrap();
        let report = validate_scoop(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("stale version reference"))
        );
    }
}
