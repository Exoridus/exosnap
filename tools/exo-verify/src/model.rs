//! Result documents exchanged between lanes, the merger and the release report.
//!
//! Every document names the bundle it describes. A document without a bundle
//! identity is a development result (quick, preflight) and can never be merged
//! into a release report.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fmt;

pub const RESULT_SCHEMA_VERSION: u32 = 1;

/// The only verdicts a scenario can produce.
///
/// `Fail` is reserved for a meaningfully exercised product path that violated
/// its contract. A harness, environment or oracle problem is `InfraError`, a
/// missing machine capability is `Unavailable`, and neither is ever a product
/// defect.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum Verdict {
    Pass,
    Fail,
    Unavailable,
    InfraError,
    Skipped,
}

impl Verdict {
    pub fn as_str(self) -> &'static str {
        match self {
            Verdict::Pass => "PASS",
            Verdict::Fail => "FAIL",
            Verdict::Unavailable => "UNAVAILABLE",
            Verdict::InfraError => "INFRA_ERROR",
            Verdict::Skipped => "SKIPPED",
        }
    }
}

impl fmt::Display for Verdict {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// One scenario outcome. `detail` is the one-line reason a human reads first;
/// `evidence` holds the measured facts the verdict was derived from.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ScenarioResult {
    pub id: String,
    pub scenario_revision: u32,
    pub verdict: Verdict,
    pub detail: String,
    #[serde(default)]
    pub duration_ms: u64,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub missing_capabilities: Vec<String>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub evidence: BTreeMap<String, serde_json::Value>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub artifacts: Vec<String>,
}

/// What was actually tested. Identity describes; it never invalidates results
/// on its own. Only `bundle_sha256` gates merging.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Identity {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bundle_sha256: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source_commit: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub product_version: Option<String>,
    /// Package file name to SHA-256 of the bytes the lane actually used.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub packages: BTreeMap<String, String>,
}

/// The document one lane emits.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaneResult {
    pub schema_version: u32,
    pub lane: String,
    pub profile: String,
    pub runner_version: String,
    pub started_at: String,
    pub finished_at: String,
    pub identity: Identity,
    #[serde(default)]
    pub environment: BTreeMap<String, serde_json::Value>,
    #[serde(default)]
    pub tools: BTreeMap<String, String>,
    #[serde(default)]
    pub capabilities: Vec<String>,
    pub scenarios: Vec<ScenarioResult>,
}

impl LaneResult {
    pub fn bundle_sha256(&self) -> Option<&str> {
        self.identity.bundle_sha256.as_deref()
    }

    pub fn count(&self, verdict: Verdict) -> usize {
        self.scenarios
            .iter()
            .filter(|s| s.verdict == verdict)
            .count()
    }
}

pub fn runner_version() -> String {
    format!("exo-verify {}", env!("CARGO_PKG_VERSION"))
}

pub fn now_rfc3339() -> String {
    time::OffsetDateTime::now_utc()
        .format(&time::format_description::well_known::Rfc3339)
        .unwrap_or_else(|_| "unknown".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn verdicts_serialize_as_their_report_names() {
        let json = serde_json::to_string(&[
            Verdict::Pass,
            Verdict::Fail,
            Verdict::Unavailable,
            Verdict::InfraError,
            Verdict::Skipped,
        ])
        .unwrap();
        assert_eq!(
            json,
            r#"["PASS","FAIL","UNAVAILABLE","INFRA_ERROR","SKIPPED"]"#
        );
    }

    #[test]
    fn unknown_verdict_is_rejected() {
        assert!(serde_json::from_str::<Verdict>(r#""UNVERIFIED""#).is_err());
    }
}
