//! The per-candidate release plan and the maintainer's explicit decisions.
//!
//! A plan is written once, when the candidate is built, from the scenario
//! registry of that source commit. It is frozen for the candidate: a scenario
//! added to the registry afterwards does not change what this candidate needs.
//! Decisions record accepted risk. They never rewrite a verdict.

use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

use crate::bundle::is_sha256;
use crate::model::Verdict;

pub const PLAN_SCHEMA: &str = "exosnap.release-plan/1";
pub const DECISIONS_SCHEMA: &str = "exosnap.release-decisions/1";

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Tier {
    /// Blocks readiness unless it passes or carries an explicit decision.
    Required,
    /// Reported, never blocking.
    Recommended,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlannedScenario {
    pub id: String,
    pub scenario_revision: u32,
    pub lane: String,
    pub tier: Tier,
    pub title: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReleasePlan {
    pub schema: String,
    pub product_version: String,
    pub bundle_sha256: String,
    pub candidate_id: String,
    pub source_commit: String,
    pub created_at: String,
    pub scenarios: Vec<PlannedScenario>,
}

impl ReleasePlan {
    pub fn load(path: &Path) -> Result<Self> {
        let plan: ReleasePlan = serde_json::from_slice(
            &fs::read(path).with_context(|| format!("read {}", path.display()))?,
        )
        .with_context(|| format!("parse {}", path.display()))?;
        ensure!(
            plan.schema == PLAN_SCHEMA,
            "unsupported plan schema '{}'",
            plan.schema
        );
        ensure!(
            is_sha256(&plan.bundle_sha256),
            "plan names a malformed bundle SHA-256"
        );
        let mut ids: Vec<&str> = plan.scenarios.iter().map(|s| s.id.as_str()).collect();
        ids.sort_unstable();
        if let Some(pair) = ids.windows(2).find(|w| w[0] == w[1]) {
            bail!("plan lists scenario '{}' twice", pair[0]);
        }
        Ok(plan)
    }

    pub fn entry(&self, id: &str) -> Option<&PlannedScenario> {
        self.scenarios.iter().find(|s| s.id == id)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum DecisionKind {
    /// A scenario that could not be judged (UNAVAILABLE, INFRA_ERROR, SKIPPED
    /// or never run) is accepted as not judged for this release.
    Accepted,
    /// A FAIL is accepted as a known, shipped risk. Only this kind covers a FAIL.
    AcceptedRisk,
}

impl DecisionKind {
    pub fn as_str(self) -> &'static str {
        match self {
            DecisionKind::Accepted => "ACCEPTED",
            DecisionKind::AcceptedRisk => "ACCEPTED_RISK",
        }
    }

    /// Whether this decision covers a scenario whose effective verdict is
    /// `verdict` (`None` when no lane reported it).
    pub fn covers(self, verdict: Option<Verdict>) -> bool {
        match (self, verdict) {
            (_, Some(Verdict::Pass)) => true,
            (DecisionKind::AcceptedRisk, _) => true,
            (DecisionKind::Accepted, Some(Verdict::Fail)) => false,
            (DecisionKind::Accepted, _) => true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Decision {
    pub scenario: String,
    pub decision: DecisionKind,
    pub reason: String,
    pub decided_by: String,
    pub decided_at: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Decisions {
    pub schema: String,
    pub bundle_sha256: String,
    pub decisions: Vec<Decision>,
}

impl Decisions {
    pub fn new(bundle_sha256: &str) -> Self {
        Decisions {
            schema: DECISIONS_SCHEMA.into(),
            bundle_sha256: bundle_sha256.into(),
            decisions: vec![],
        }
    }

    pub fn load(path: &Path) -> Result<Self> {
        let decisions: Decisions = serde_json::from_slice(
            &fs::read(path).with_context(|| format!("read {}", path.display()))?,
        )
        .with_context(|| format!("parse {}", path.display()))?;
        ensure!(
            decisions.schema == DECISIONS_SCHEMA,
            "unsupported decisions schema '{}'",
            decisions.schema
        );
        ensure!(
            is_sha256(&decisions.bundle_sha256),
            "decisions name a malformed bundle SHA-256"
        );
        Ok(decisions)
    }

    pub fn save(&self, path: &Path) -> Result<()> {
        let mut bytes = serde_json::to_vec_pretty(self)?;
        bytes.push(b'\n');
        fs::write(path, bytes)?;
        Ok(())
    }

    /// Records a decision, replacing an earlier one for the same scenario.
    pub fn record(&mut self, decision: Decision) -> Result<()> {
        ensure!(
            decision.reason.trim().len() >= 10,
            "a decision needs a reason a reviewer can act on (at least 10 characters)"
        );
        self.decisions.retain(|d| d.scenario != decision.scenario);
        self.decisions.push(decision);
        self.decisions.sort_by(|a, b| a.scenario.cmp(&b.scenario));
        Ok(())
    }

    pub fn for_scenario(&self, id: &str) -> Option<&Decision> {
        self.decisions.iter().find(|d| d.scenario == id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepted_never_covers_a_fail() {
        assert!(!DecisionKind::Accepted.covers(Some(Verdict::Fail)));
        assert!(DecisionKind::AcceptedRisk.covers(Some(Verdict::Fail)));
        assert!(DecisionKind::Accepted.covers(Some(Verdict::Unavailable)));
        assert!(DecisionKind::Accepted.covers(None));
    }

    #[test]
    fn a_decision_without_a_reason_is_refused() {
        let mut d = Decisions::new(&"a".repeat(64));
        let error = d
            .record(Decision {
                scenario: "x".into(),
                decision: DecisionKind::Accepted,
                reason: "ok".into(),
                decided_by: "m".into(),
                decided_at: "t".into(),
            })
            .unwrap_err();
        assert!(error.to_string().contains("reason"));
    }
}
