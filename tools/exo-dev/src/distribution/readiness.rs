//! The distribution readiness report: the machine-readable binding between a
//! prepared package set and the immutable public release, plus its human
//! rendering and its frozen digest.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use anyhow::{Context as _, Result, ensure};
use serde::{Deserialize, Serialize};

use super::release::{DistributionRelease, sha256_hex};

pub const READINESS_SCHEMA: &str = "exosnap.distribution-readiness/1";
pub const READINESS_NAME: &str = "distribution-readiness.json";
pub const READINESS_MARKDOWN: &str = "distribution-readiness.md";
pub const READINESS_SIDECAR: &str = "distribution-readiness.json.sha256";

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReleaseRef {
    pub id: u64,
    pub url: String,
    pub published_at: String,
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChannelReadiness {
    pub state: String,
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub msi_url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub msi_sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub zip_url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub zip_sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub product_code: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub package_filename: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub package_sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub manifest_sha256: Option<String>,
    pub checks: BTreeMap<String, bool>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub detail: Vec<String>,
}

impl ChannelReadiness {
    pub fn state(&self) -> &'static str {
        if self.checks.values().all(|passed| *passed) {
            "READY"
        } else {
            "NOT_READY"
        }
    }

    fn finalize(&mut self) {
        self.state = self.state().to_string();
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReadinessReport {
    pub schema: String,
    pub product: String,
    pub version: String,
    pub tag: String,
    pub source_commit: String,
    pub release: ReleaseRef,
    pub channels: BTreeMap<String, ChannelReadiness>,
    pub ready_for_distribution: bool,
}

impl ReadinessReport {
    pub fn new(release: &DistributionRelease) -> ReadinessReport {
        ReadinessReport {
            schema: READINESS_SCHEMA.to_string(),
            product: "ExoSnap".to_string(),
            version: release.version.clone(),
            tag: release.tag.clone(),
            source_commit: release.source_commit.clone(),
            release: ReleaseRef {
                id: release.release_id,
                url: release.release_url.clone(),
                published_at: release.published_at.clone(),
            },
            channels: BTreeMap::new(),
            ready_for_distribution: false,
        }
    }

    pub fn finalize(&mut self) {
        for channel in self.channels.values_mut() {
            channel.finalize();
        }
        self.ready_for_distribution = self
            .channels
            .values()
            .all(|channel| channel.state == "READY");
    }

    pub fn load(path: &Path) -> Result<ReadinessReport> {
        let report: ReadinessReport = serde_json::from_slice(
            &fs::read(path).with_context(|| format!("could not read {}", path.display()))?,
        )
        .with_context(|| format!("{} is not a readiness report", path.display()))?;
        ensure!(
            report.schema == READINESS_SCHEMA,
            "unsupported readiness schema '{}'",
            report.schema
        );
        Ok(report)
    }

    pub fn write(&self, prepared: &Path) -> Result<String> {
        let bytes = serde_json::to_vec_pretty(self)?;
        fs::write(prepared.join(READINESS_NAME), &bytes)?;
        fs::write(prepared.join(READINESS_MARKDOWN), self.render_markdown())?;
        let digest = sha256_hex(&bytes);
        fs::write(
            prepared.join(READINESS_SIDECAR),
            format!("{digest}  {READINESS_NAME}\n"),
        )?;
        Ok(digest)
    }

    /// The frozen digest recorded beside the report, re-read from disk.
    pub fn frozen_digest(prepared: &Path) -> Result<String> {
        let sidecar = fs::read_to_string(prepared.join(READINESS_SIDECAR)).with_context(|| {
            format!("no frozen {READINESS_SIDECAR} under {}", prepared.display())
        })?;
        let digest = sidecar
            .split_whitespace()
            .next()
            .unwrap_or_default()
            .to_string();
        let bytes = fs::read(prepared.join(READINESS_NAME))?;
        ensure!(
            sha256_hex(&bytes) == digest,
            "the frozen readiness digest does not match the readiness report"
        );
        Ok(digest)
    }

    pub fn render_markdown(&self) -> String {
        let mut out = format!("# ExoSnap {} distribution readiness\n\n", self.version);
        out.push_str(&format!(
            "GitHub release: {} (`{}`), source `{}`.\n\n",
            self.release.url, self.tag, self.source_commit
        ));
        out.push_str("| Channel | State | MSI / ZIP | SHA-256 | Checks |\n");
        out.push_str("|---|---|---|---|---|\n");
        for (name, channel) in &self.channels {
            let artifact = if let Some(url) = &channel.msi_url {
                url.clone()
            } else if let Some(url) = &channel.zip_url {
                url.clone()
            } else {
                "-".to_string()
            };
            let digest = channel
                .msi_sha256
                .as_deref()
                .or(channel.zip_sha256.as_deref())
                .or(channel.package_sha256.as_deref())
                .unwrap_or("-");
            let checks: Vec<String> = channel
                .checks
                .iter()
                .map(|(name, ok)| format!("{name}={}", if *ok { "pass" } else { "FAIL" }))
                .collect();
            out.push_str(&format!(
                "| {name} | {} | {} | `{digest}` | {} |\n",
                channel.state,
                artifact,
                checks.join(", ")
            ));
        }
        out.push('\n');
        for (name, channel) in &self.channels {
            for line in &channel.detail {
                out.push_str(&format!("- {name}: {line}\n"));
            }
        }
        if self.ready_for_distribution {
            out.push_str(
                "\n**READY FOR DISTRIBUTION.** Every selected channel is prepared and validated.\n",
            );
        } else {
            out.push_str("\n**NOT READY.** At least one selected channel is not ready.\n");
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn release() -> DistributionRelease {
        DistributionRelease {
            version: "0.10.0".into(),
            tag: "v0.10.0".into(),
            source_commit: "a".repeat(40),
            release_id: 1,
            release_url: "https://example.invalid".into(),
            published_at: "2026-10-02T16:28:12Z".into(),
            msi: super::super::release::PackageAsset {
                filename: "x.msi".into(),
                url: "https://example.invalid/x.msi".into(),
                sha256: "b".repeat(64),
                size: 1,
            },
            portable: super::super::release::PackageAsset {
                filename: "x.zip".into(),
                url: "https://example.invalid/x.zip".into(),
                sha256: "c".repeat(64),
                size: 1,
            },
        }
    }

    #[test]
    fn a_report_is_only_ready_when_every_channel_passed_every_check() {
        let mut report = ReadinessReport::new(&release());
        report.channels.insert(
            "chocolatey".into(),
            ChannelReadiness {
                version: "0.10.0".into(),
                checks: BTreeMap::from([
                    ("packageValidation".to_string(), true),
                    ("pack".to_string(), true),
                    ("rehearsal".to_string(), false),
                ]),
                ..Default::default()
            },
        );
        report.finalize();
        assert!(!report.ready_for_distribution);
        assert_eq!(report.channels["chocolatey"].state, "NOT_READY");

        report
            .channels
            .get_mut("chocolatey")
            .unwrap()
            .checks
            .insert("rehearsal".into(), true);
        report.finalize();
        assert!(report.ready_for_distribution);
        assert!(report.render_markdown().contains("READY FOR DISTRIBUTION"));
    }

    #[test]
    fn the_frozen_digest_detects_an_altered_report() {
        let dir = tempfile::tempdir().unwrap();
        let mut report = ReadinessReport::new(&release());
        report.finalize();
        let digest = report.write(dir.path()).unwrap();
        assert_eq!(ReadinessReport::frozen_digest(dir.path()).unwrap(), digest);
        let path = dir.path().join(READINESS_NAME);
        let mut text = fs::read_to_string(&path).unwrap();
        text.push(' ');
        fs::write(&path, text).unwrap();
        assert!(ReadinessReport::frozen_digest(dir.path()).is_err());
    }
}
