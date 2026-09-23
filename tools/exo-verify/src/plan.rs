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

use crate::bundle::{Bundle, is_sha256};
use crate::model::Verdict;
use crate::scenario::Scenario;

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

    /// Re-derives the frozen plan from the candidate's source registry before
    /// release approval. A supplied plan cannot omit or demote a required check.
    pub fn verify_against(&self, bundle: &Bundle, registry: &[Scenario]) -> Result<()> {
        ensure!(
            self.schema == PLAN_SCHEMA,
            "unsupported plan schema '{}'",
            self.schema
        );
        ensure!(
            self.bundle_sha256 == bundle.sha256
                && self.product_version == bundle.inventory.product_version
                && self.candidate_id == bundle.inventory.candidate_id
                && self.source_commit == bundle.inventory.source_commit,
            "plan identity differs from the candidate bundle"
        );
        let mut expected: Vec<PlannedScenario> = registry
            .iter()
            .filter(|s| s.lane.is_release())
            .map(|s| PlannedScenario {
                id: s.id.into(),
                scenario_revision: s.revision,
                lane: s.lane.name().into(),
                tier: s.tier,
                title: s.title.into(),
            })
            .collect();
        let mut actual = self.scenarios.clone();
        expected.sort_by(|a, b| a.id.cmp(&b.id));
        actual.sort_by(|a, b| a.id.cmp(&b.id));
        ensure!(
            actual == expected,
            "plan scenarios differ from the source registry"
        );
        Ok(())
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
            (DecisionKind::AcceptedRisk, Some(Verdict::Fail)) => true,
            (DecisionKind::AcceptedRisk, _) => false,
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
        let mut seen = std::collections::BTreeSet::new();
        for decision in &decisions.decisions {
            ensure!(
                seen.insert(&decision.scenario),
                "decisions list scenario '{}' twice",
                decision.scenario
            );
            ensure!(
                decision.reason.trim().len() >= 10,
                "decision for '{}' has no reviewable reason",
                decision.scenario
            );
            ensure!(
                !decision.decided_by.trim().is_empty(),
                "decision for '{}' has no author",
                decision.scenario
            );
        }
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
        ensure!(
            !decision.decided_by.trim().is_empty(),
            "a decision needs an author"
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
    use crate::bundle::{BUNDLE_SCHEMA, Bundle, Inventory};
    use crate::capability::Capability;
    use crate::scenario::{Lane, Scenario, Step};
    use std::path::PathBuf;
    use std::time::Duration;

    fn no_op(_: &mut crate::context::Context) -> Step {
        Ok(())
    }

    fn fixture_bundle() -> Bundle {
        Bundle {
            root: PathBuf::new(),
            sha256: "a".repeat(64),
            inventory: Inventory {
                schema: BUNDLE_SCHEMA.into(),
                product: "ExoSnap".into(),
                product_version: "0.10.0".into(),
                source_commit: "b".repeat(40),
                candidate_id: "candidate-1".into(),
                created_at: "t".into(),
                toolchain: Default::default(),
                inputs: Default::default(),
                files: vec![],
            },
        }
    }

    fn fixture_registry() -> Vec<Scenario> {
        vec![Scenario {
            id: "core.required",
            revision: 3,
            title: "Required check",
            claim: "The required claim holds",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: Duration::from_secs(1),
            run: no_op,
        }]
    }

    fn fixture_plan() -> ReleasePlan {
        let bundle = fixture_bundle();
        ReleasePlan {
            schema: PLAN_SCHEMA.into(),
            product_version: bundle.inventory.product_version,
            bundle_sha256: bundle.sha256,
            candidate_id: bundle.inventory.candidate_id,
            source_commit: bundle.inventory.source_commit,
            created_at: "t".into(),
            scenarios: vec![PlannedScenario {
                id: "core.required".into(),
                scenario_revision: 3,
                lane: Lane::CiCore.name().into(),
                tier: Tier::Required,
                title: "Required check".into(),
            }],
        }
    }

    #[test]
    fn a_plan_must_match_the_bundle_and_source_registry() {
        let bundle = fixture_bundle();
        let registry = fixture_registry();
        let plan = fixture_plan();
        plan.verify_against(&bundle, &registry).unwrap();

        let mut missing = plan.clone();
        missing.scenarios.clear();
        assert!(missing.verify_against(&bundle, &registry).is_err());

        let mut moved = plan.clone();
        moved.scenarios[0].lane = Lane::Gpu.name().into();
        assert!(moved.verify_against(&bundle, &registry).is_err());

        let mut wrong_commit = plan;
        wrong_commit.source_commit = "c".repeat(40);
        assert!(wrong_commit.verify_against(&bundle, &registry).is_err());
    }

    #[test]
    fn accepted_never_covers_a_fail() {
        assert!(!DecisionKind::Accepted.covers(Some(Verdict::Fail)));
        assert!(DecisionKind::AcceptedRisk.covers(Some(Verdict::Fail)));
        assert!(DecisionKind::Accepted.covers(Some(Verdict::Unavailable)));
        assert!(DecisionKind::Accepted.covers(None));
    }

    #[test]
    fn accepted_risk_requires_an_observed_product_failure() {
        assert!(DecisionKind::AcceptedRisk.covers(Some(Verdict::Fail)));
        assert!(!DecisionKind::AcceptedRisk.covers(None));
        assert!(!DecisionKind::AcceptedRisk.covers(Some(Verdict::Unavailable)));
        assert!(!DecisionKind::AcceptedRisk.covers(Some(Verdict::InfraError)));
    }

    #[test]
    fn loaded_decisions_reject_duplicate_or_reasonless_entries() {
        let path = std::env::temp_dir().join(format!(
            "exo-verify-decisions-{}.json",
            crate::control::new_run_id("test")
        ));
        let mut decisions = Decisions::new(&"a".repeat(64));
        decisions
            .record(Decision {
                scenario: "core.required".into(),
                decision: DecisionKind::Accepted,
                reason: "An explicit reason".into(),
                decided_by: "maintainer".into(),
                decided_at: "now".into(),
            })
            .unwrap();
        decisions.decisions.push(decisions.decisions[0].clone());
        decisions.save(&path).unwrap();
        assert!(Decisions::load(&path).is_err());
        decisions.decisions.pop();
        decisions.decisions[0].reason.clear();
        decisions.save(&path).unwrap();
        assert!(Decisions::load(&path).is_err());
        std::fs::remove_file(path).unwrap();
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
