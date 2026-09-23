//! Merging lane results into one release report.
//!
//! The merge refuses any result that names a different bundle than the plan.
//! A scenario reported by several runs takes the most severe product verdict:
//! a FAIL in one run and a PASS in another is a FAIL, never "flaky but green".
//! An INFRA_ERROR followed by a PASS is a PASS, because the infrastructure
//! failure never judged the product.

use anyhow::{Result, bail};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fmt::Write as _;

use crate::model::{LaneResult, ScenarioResult, Verdict};
use crate::plan::{Decision, DecisionKind, Decisions, ReleasePlan, Tier};

pub const REPORT_SCHEMA: &str = "exosnap.release-report/1";

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Run {
    pub lane: String,
    pub runner_version: String,
    pub finished_at: String,
    pub verdict: Verdict,
    pub scenario_revision: u32,
    pub detail: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportEntry {
    pub id: String,
    pub title: String,
    pub lane: String,
    pub tier: Tier,
    pub planned_revision: u32,
    /// Effective verdict over all runs of the planned revision; `None` when no
    /// run of that revision was reported.
    pub verdict: Option<Verdict>,
    pub detail: String,
    pub runs: Vec<Run>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub decision: Option<Decision>,
    pub satisfied: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LaneSummary {
    pub lane: String,
    pub runner_version: String,
    pub started_at: String,
    pub finished_at: String,
    pub environment: BTreeMap<String, serde_json::Value>,
    pub tools: BTreeMap<String, String>,
    pub counts: BTreeMap<String, usize>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Report {
    pub schema: String,
    pub product_version: String,
    pub candidate_id: String,
    pub source_commit: String,
    pub bundle_sha256: String,
    pub generated_at: String,
    pub ready_for_approval: bool,
    pub blocking: Vec<String>,
    pub lanes: Vec<LaneSummary>,
    pub scenarios: Vec<ReportEntry>,
    /// Results for scenarios the plan does not list. Shown, never counted.
    pub unplanned: Vec<ScenarioResult>,
}

fn severity(verdict: Verdict) -> u8 {
    match verdict {
        Verdict::Fail => 5,
        Verdict::Pass => 4,
        Verdict::InfraError => 3,
        Verdict::Unavailable => 2,
        Verdict::Skipped => 1,
    }
}

pub fn effective(verdicts: impl IntoIterator<Item = Verdict>) -> Option<Verdict> {
    verdicts.into_iter().max_by_key(|v| severity(*v))
}

pub fn merge(
    plan: &ReleasePlan,
    results: &[LaneResult],
    decisions: Option<&Decisions>,
) -> Result<Report> {
    for result in results {
        match result.bundle_sha256() {
            Some(sha) if sha == plan.bundle_sha256 => {}
            Some(sha) => bail!(
                "lane '{}' ({}) tested bundle {sha}, but the plan is for bundle {}; results for different bytes are never merged",
                result.lane,
                result.finished_at,
                plan.bundle_sha256
            ),
            None => bail!(
                "lane '{}' ({}) names no bundle; development results (quick, preflight) are not release evidence",
                result.lane,
                result.finished_at
            ),
        }
    }
    if let Some(decisions) = decisions
        && decisions.bundle_sha256 != plan.bundle_sha256
    {
        bail!(
            "the decisions were recorded for bundle {}, not for this candidate's bundle {}",
            decisions.bundle_sha256,
            plan.bundle_sha256
        );
    }

    let mut entries = Vec::new();
    let mut blocking = Vec::new();
    for planned in &plan.scenarios {
        let mut runs = Vec::new();
        for result in results {
            for scenario in result.scenarios.iter().filter(|s| s.id == planned.id) {
                runs.push(Run {
                    lane: result.lane.clone(),
                    runner_version: result.runner_version.clone(),
                    finished_at: result.finished_at.clone(),
                    verdict: scenario.verdict,
                    scenario_revision: scenario.scenario_revision,
                    detail: scenario.detail.clone(),
                });
            }
        }
        let current: Vec<&Run> = runs
            .iter()
            .filter(|r| r.scenario_revision == planned.scenario_revision)
            .collect();
        let verdict = effective(current.iter().map(|r| r.verdict));
        let detail = match verdict {
            Some(v) => current
                .iter()
                .find(|r| r.verdict == v)
                .map(|r| r.detail.clone())
                .unwrap_or_default(),
            None if !runs.is_empty() => format!(
                "only other scenario revisions were reported ({}); the plan requires revision {}",
                runs.iter()
                    .map(|r| r.scenario_revision.to_string())
                    .collect::<Vec<_>>()
                    .join(", "),
                planned.scenario_revision
            ),
            None => "not run".to_string(),
        };
        let decision = decisions.and_then(|d| d.for_scenario(&planned.id)).cloned();
        let satisfied = verdict == Some(Verdict::Pass)
            || decision
                .as_ref()
                .is_some_and(|d| d.decision.covers(verdict));
        if planned.tier == Tier::Required && !satisfied {
            let why = match (verdict, &decision) {
                (Some(Verdict::Fail), Some(d)) if d.decision == DecisionKind::Accepted => {
                    "FAIL; an ACCEPTED decision does not cover a FAIL, only ACCEPTED_RISK does"
                        .to_string()
                }
                (Some(v), _) => v.to_string(),
                (None, _) => "not run".to_string(),
            };
            blocking.push(format!("{} ({})", planned.id, why));
        }
        entries.push(ReportEntry {
            id: planned.id.clone(),
            title: planned.title.clone(),
            lane: planned.lane.clone(),
            tier: planned.tier,
            planned_revision: planned.scenario_revision,
            verdict,
            detail,
            runs,
            decision,
            satisfied,
        });
    }

    let unplanned = results
        .iter()
        .flat_map(|r| r.scenarios.iter())
        .filter(|s| plan.entry(&s.id).is_none())
        .cloned()
        .collect();

    let lanes = results
        .iter()
        .map(|r| {
            let mut counts = BTreeMap::new();
            for v in [
                Verdict::Pass,
                Verdict::Fail,
                Verdict::Unavailable,
                Verdict::InfraError,
                Verdict::Skipped,
            ] {
                counts.insert(v.to_string(), r.count(v));
            }
            LaneSummary {
                lane: r.lane.clone(),
                runner_version: r.runner_version.clone(),
                started_at: r.started_at.clone(),
                finished_at: r.finished_at.clone(),
                environment: r.environment.clone(),
                tools: r.tools.clone(),
                counts,
            }
        })
        .collect();

    Ok(Report {
        schema: REPORT_SCHEMA.into(),
        product_version: plan.product_version.clone(),
        candidate_id: plan.candidate_id.clone(),
        source_commit: plan.source_commit.clone(),
        bundle_sha256: plan.bundle_sha256.clone(),
        generated_at: crate::model::now_rfc3339(),
        ready_for_approval: blocking.is_empty(),
        blocking,
        lanes,
        scenarios: entries,
        unplanned,
    })
}

fn verdict_cell(entry: &ReportEntry) -> String {
    match entry.verdict {
        Some(v) => v.to_string(),
        None => "NOT RUN".into(),
    }
}

fn one_line(text: &str) -> String {
    text.replace('|', "\\|").replace(['\r', '\n'], " ")
}

pub fn markdown(report: &Report) -> String {
    let mut md = String::new();
    let _ = writeln!(md, "# ExoSnap {} release report", report.product_version);
    let _ = writeln!(md);
    if report.ready_for_approval {
        let _ = writeln!(
            md,
            "**Ready for human publication approval.** Every required scenario passed or carries an explicit maintainer decision. This report does not authorize publication; approving the `release` environment does."
        );
    } else {
        let _ = writeln!(
            md,
            "**Not ready.** {} required scenario(s) are unresolved:",
            report.blocking.len()
        );
        let _ = writeln!(md);
        for item in &report.blocking {
            let _ = writeln!(md, "- {item}");
        }
    }
    let _ = writeln!(md);
    let _ = writeln!(md, "| Candidate | Value |");
    let _ = writeln!(md, "|---|---|");
    let _ = writeln!(md, "| Version | {} |", report.product_version);
    let _ = writeln!(md, "| Candidate | {} |", report.candidate_id);
    let _ = writeln!(md, "| Source commit | `{}` |", report.source_commit);
    let _ = writeln!(md, "| Bundle SHA-256 | `{}` |", report.bundle_sha256);
    let _ = writeln!(md);

    let _ = writeln!(md, "## Lanes");
    let _ = writeln!(md);
    if report.lanes.is_empty() {
        let _ = writeln!(md, "No lane has reported yet.");
    } else {
        let _ = writeln!(
            md,
            "| Lane | Finished | PASS | FAIL | UNAVAILABLE | INFRA_ERROR | SKIPPED | Runner |"
        );
        let _ = writeln!(md, "|---|---|---|---|---|---|---|---|");
        for lane in &report.lanes {
            let c = |k: &str| lane.counts.get(k).copied().unwrap_or(0);
            let _ = writeln!(
                md,
                "| {} | {} | {} | {} | {} | {} | {} | {} |",
                lane.lane,
                lane.finished_at,
                c("PASS"),
                c("FAIL"),
                c("UNAVAILABLE"),
                c("INFRA_ERROR"),
                c("SKIPPED"),
                lane.runner_version
            );
        }
    }
    let _ = writeln!(md);

    let decided: Vec<&ReportEntry> = report
        .scenarios
        .iter()
        .filter(|e| e.decision.is_some())
        .collect();
    let _ = writeln!(md, "## Maintainer decisions");
    let _ = writeln!(md);
    if decided.is_empty() {
        let _ = writeln!(md, "None.");
    } else {
        let _ = writeln!(md, "| Scenario | Verdict | Decision | Reason | By |");
        let _ = writeln!(md, "|---|---|---|---|---|");
        for e in decided {
            let d = e.decision.as_ref().unwrap();
            let _ = writeln!(
                md,
                "| {} | {} | {} | {} | {} |",
                e.id,
                verdict_cell(e),
                d.decision.as_str(),
                one_line(&d.reason),
                one_line(&d.decided_by)
            );
        }
    }
    let _ = writeln!(md);

    for (heading, tier) in [
        ("Required scenarios", Tier::Required),
        ("Recommended scenarios", Tier::Recommended),
    ] {
        let rows: Vec<&ReportEntry> = report.scenarios.iter().filter(|e| e.tier == tier).collect();
        if rows.is_empty() {
            continue;
        }
        let _ = writeln!(md, "## {heading}");
        let _ = writeln!(md);
        let _ = writeln!(md, "| Scenario | Lane | Verdict | Detail |");
        let _ = writeln!(md, "|---|---|---|---|");
        for e in rows {
            let _ = writeln!(
                md,
                "| {} | {} | {} | {} |",
                e.id,
                e.lane,
                verdict_cell(e),
                one_line(&e.detail)
            );
        }
        let _ = writeln!(md);
    }
    if !report.unplanned.is_empty() {
        let _ = writeln!(md, "## Results outside the plan");
        let _ = writeln!(md);
        let _ = writeln!(
            md,
            "These scenarios are not part of this candidate's plan and do not affect readiness."
        );
        let _ = writeln!(md);
        for s in &report.unplanned {
            let _ = writeln!(md, "- {}: {} ({})", s.id, s.verdict, one_line(&s.detail));
        }
    }
    md
}

fn xml_escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            c if (c as u32) < 0x20 && !matches!(c, '\t' | '\n' | '\r') => out.push(' '),
            c => out.push(c),
        }
    }
    out
}

/// JUnit view of the report. An accepted FAIL stays a failure here; the
/// decision is recorded as a property, so no CI dashboard shows it green.
pub fn junit(report: &Report) -> String {
    let mut by_lane: BTreeMap<&str, Vec<&ReportEntry>> = BTreeMap::new();
    for e in &report.scenarios {
        by_lane.entry(e.lane.as_str()).or_default().push(e);
    }
    let mut xml = String::from("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    let total = report.scenarios.len();
    let failures = report
        .scenarios
        .iter()
        .filter(|e| e.verdict == Some(Verdict::Fail))
        .count();
    let errors = report
        .scenarios
        .iter()
        .filter(|e| e.verdict == Some(Verdict::InfraError))
        .count();
    let _ = writeln!(
        xml,
        "<testsuites name=\"exosnap-release {}\" tests=\"{total}\" failures=\"{failures}\" errors=\"{errors}\">",
        xml_escape(&report.product_version)
    );
    for (lane, entries) in by_lane {
        let f = entries
            .iter()
            .filter(|e| e.verdict == Some(Verdict::Fail))
            .count();
        let err = entries
            .iter()
            .filter(|e| e.verdict == Some(Verdict::InfraError))
            .count();
        let skipped = entries
            .iter()
            .filter(|e| {
                matches!(
                    e.verdict,
                    None | Some(Verdict::Unavailable) | Some(Verdict::Skipped)
                )
            })
            .count();
        let _ = writeln!(
            xml,
            "  <testsuite name=\"{}\" tests=\"{}\" failures=\"{f}\" errors=\"{err}\" skipped=\"{skipped}\">",
            xml_escape(lane),
            entries.len()
        );
        for e in entries {
            let _ = writeln!(
                xml,
                "    <testcase classname=\"{}\" name=\"{}\">",
                xml_escape(lane),
                xml_escape(&e.id)
            );
            if let Some(d) = &e.decision {
                let _ = writeln!(
                    xml,
                    "      <properties><property name=\"decision\" value=\"{}: {}\"/></properties>",
                    d.decision.as_str(),
                    xml_escape(&d.reason)
                );
            }
            let message = xml_escape(&e.detail);
            match e.verdict {
                Some(Verdict::Pass) => {}
                Some(Verdict::Fail) => {
                    let _ = writeln!(xml, "      <failure message=\"{message}\"/>");
                }
                Some(Verdict::InfraError) => {
                    let _ = writeln!(xml, "      <error message=\"{message}\"/>");
                }
                Some(v) => {
                    let _ = writeln!(xml, "      <skipped message=\"{}: {message}\"/>", v);
                }
                None => {
                    let _ = writeln!(xml, "      <skipped message=\"NOT RUN\"/>");
                }
            }
            let _ = writeln!(xml, "    </testcase>");
        }
        let _ = writeln!(xml, "  </testsuite>");
    }
    xml.push_str("</testsuites>\n");
    xml
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Identity, LaneResult, RESULT_SCHEMA_VERSION};
    use crate::plan::{PLAN_SCHEMA, PlannedScenario};

    const SHA: &str = "1111111111111111111111111111111111111111111111111111111111111111";
    const OTHER: &str = "2222222222222222222222222222222222222222222222222222222222222222";

    fn plan() -> ReleasePlan {
        let s = |id: &str, tier| PlannedScenario {
            id: id.into(),
            scenario_revision: 1,
            lane: "release-ci-core".into(),
            tier,
            title: id.into(),
        };
        ReleasePlan {
            schema: PLAN_SCHEMA.into(),
            product_version: "0.10.0".into(),
            bundle_sha256: SHA.into(),
            candidate_id: "c".into(),
            source_commit: "0".repeat(40),
            created_at: "t".into(),
            scenarios: vec![
                s("a", Tier::Required),
                s("b", Tier::Required),
                s("c", Tier::Recommended),
            ],
        }
    }

    fn lane(sha: Option<&str>, verdicts: &[(&str, Verdict)]) -> LaneResult {
        LaneResult {
            schema_version: RESULT_SCHEMA_VERSION,
            lane: "release-ci-core".into(),
            profile: "release-ci".into(),
            runner_version: "exo-verify test".into(),
            started_at: "s".into(),
            finished_at: "f".into(),
            identity: Identity {
                bundle_sha256: sha.map(String::from),
                ..Default::default()
            },
            environment: BTreeMap::new(),
            tools: BTreeMap::new(),
            capabilities: vec![],
            scenarios: verdicts
                .iter()
                .map(|(id, v)| ScenarioResult {
                    id: id.to_string(),
                    scenario_revision: 1,
                    verdict: *v,
                    detail: format!("{id} {v}"),
                    duration_ms: 1,
                    missing_capabilities: vec![],
                    evidence: BTreeMap::new(),
                    artifacts: vec![],
                })
                .collect(),
        }
    }

    fn decision(id: &str, kind: DecisionKind) -> Decisions {
        let mut d = Decisions::new(SHA);
        d.record(Decision {
            scenario: id.into(),
            decision: kind,
            reason: "rig unavailable for this release".into(),
            decided_by: "maintainer".into(),
            decided_at: "t".into(),
        })
        .unwrap();
        d
    }

    #[test]
    fn results_for_another_bundle_cannot_be_merged() {
        let error =
            merge(&plan(), &[lane(Some(OTHER), &[("a", Verdict::Pass)])], None).unwrap_err();
        assert!(error.to_string().contains("never merged"), "{error}");
    }

    #[test]
    fn development_results_without_bundle_identity_cannot_be_merged() {
        let error = merge(&plan(), &[lane(None, &[("a", Verdict::Pass)])], None).unwrap_err();
        assert!(
            error.to_string().contains("not release evidence"),
            "{error}"
        );
    }

    #[test]
    fn decisions_for_another_bundle_are_refused() {
        let mut d = decision("a", DecisionKind::Accepted);
        d.bundle_sha256 = OTHER.into();
        assert!(merge(&plan(), &[], Some(&d)).is_err());
    }

    #[test]
    fn all_required_passing_is_ready() {
        let r = merge(
            &plan(),
            &[lane(
                Some(SHA),
                &[("a", Verdict::Pass), ("b", Verdict::Pass)],
            )],
            None,
        )
        .unwrap();
        assert!(r.ready_for_approval);
        assert_eq!(
            r.scenarios[2].verdict, None,
            "recommended not run stays visible"
        );
    }

    #[test]
    fn a_missing_required_result_blocks() {
        let r = merge(&plan(), &[lane(Some(SHA), &[("a", Verdict::Pass)])], None).unwrap();
        assert!(!r.ready_for_approval);
        assert_eq!(r.blocking, vec!["b (not run)".to_string()]);
    }

    #[test]
    fn fail_in_any_run_beats_pass_in_another() {
        let r = merge(
            &plan(),
            &[
                lane(Some(SHA), &[("a", Verdict::Pass), ("b", Verdict::Pass)]),
                lane(Some(SHA), &[("a", Verdict::Fail)]),
            ],
            None,
        )
        .unwrap();
        assert_eq!(r.scenarios[0].verdict, Some(Verdict::Fail));
        assert!(!r.ready_for_approval);
    }

    #[test]
    fn infra_error_then_pass_is_pass() {
        let r = merge(
            &plan(),
            &[
                lane(
                    Some(SHA),
                    &[("a", Verdict::InfraError), ("b", Verdict::Pass)],
                ),
                lane(Some(SHA), &[("a", Verdict::Pass)]),
            ],
            None,
        )
        .unwrap();
        assert!(r.ready_for_approval);
    }

    #[test]
    fn accepted_fail_remains_fail_everywhere() {
        let results = [lane(
            Some(SHA),
            &[("a", Verdict::Fail), ("b", Verdict::Pass)],
        )];
        let accepted = merge(
            &plan(),
            &results,
            Some(&decision("a", DecisionKind::Accepted)),
        )
        .unwrap();
        assert!(!accepted.ready_for_approval, "ACCEPTED does not cover FAIL");
        assert!(accepted.blocking[0].contains("only ACCEPTED_RISK"));

        let risk = merge(
            &plan(),
            &results,
            Some(&decision("a", DecisionKind::AcceptedRisk)),
        )
        .unwrap();
        assert!(risk.ready_for_approval);
        assert_eq!(
            risk.scenarios[0].verdict,
            Some(Verdict::Fail),
            "the verdict is never rewritten"
        );
        let md = markdown(&risk);
        assert!(md.contains("| a | FAIL | ACCEPTED_RISK |"), "{md}");
        let xml = junit(&risk);
        assert!(xml.contains("<failure"), "{xml}");
        assert!(xml.contains("ACCEPTED_RISK"));
    }

    #[test]
    fn unavailable_needs_a_decision() {
        let results = [lane(
            Some(SHA),
            &[("a", Verdict::Unavailable), ("b", Verdict::Pass)],
        )];
        assert!(!merge(&plan(), &results, None).unwrap().ready_for_approval);
        let r = merge(
            &plan(),
            &results,
            Some(&decision("a", DecisionKind::Accepted)),
        )
        .unwrap();
        assert!(r.ready_for_approval);
        assert_eq!(r.scenarios[0].verdict, Some(Verdict::Unavailable));
    }

    #[test]
    fn a_different_scenario_revision_does_not_satisfy_the_plan() {
        let mut result = lane(Some(SHA), &[("a", Verdict::Pass), ("b", Verdict::Pass)]);
        result.scenarios[0].scenario_revision = 2;
        let r = merge(&plan(), &[result], None).unwrap();
        assert!(!r.ready_for_approval);
        assert!(r.scenarios[0].detail.contains("requires revision 1"));
    }
}
