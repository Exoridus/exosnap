//! Runs a lane's scenarios and turns every outcome into exactly one verdict.

use anyhow::Result;
use std::collections::BTreeMap;
use std::panic::AssertUnwindSafe;
use std::time::Instant;

use crate::context::Context;
use crate::model::{
    Identity, LaneResult, RESULT_SCHEMA_VERSION, ScenarioResult, Verdict, now_rfc3339,
    runner_version,
};
use crate::scenario::{Lane, Scenario, Stop};

pub struct Selection<'a> {
    pub lane: Lane,
    pub only: &'a [String],
    pub skip: &'a [String],
}

pub fn selected<'a>(registry: &'a [Scenario], selection: &Selection) -> Vec<&'a Scenario> {
    registry
        .iter()
        .filter(|s| s.runs_in(selection.lane))
        .filter(|s| selection.only.is_empty() || selection.only.iter().any(|o| o == s.id))
        .collect()
}

pub fn run_one(scenario: &Scenario, ctx: &mut Context) -> ScenarioResult {
    let started = Instant::now();
    let mut result = ScenarioResult {
        id: scenario.id.to_string(),
        scenario_revision: scenario.revision,
        verdict: Verdict::Skipped,
        detail: String::new(),
        duration_ms: 0,
        missing_capabilities: Vec::new(),
        evidence: BTreeMap::new(),
        artifacts: Vec::new(),
    };
    let missing = ctx.caps.missing(scenario.requires);
    if !missing.is_empty() {
        result.verdict = Verdict::Unavailable;
        result.missing_capabilities = missing.iter().map(|c| c.name().to_string()).collect();
        result.detail = format!(
            "missing capability: {}",
            result.missing_capabilities.join(", ")
        );
        return result;
    }
    if let Err(e) = ctx.begin(scenario.id) {
        result.verdict = Verdict::InfraError;
        result.detail = format!("could not prepare scenario directory: {e:#}");
        return result;
    }

    let outcome = std::panic::catch_unwind(AssertUnwindSafe(|| (scenario.run)(ctx)));
    let cleanup = ctx.job.terminate();
    let (mut verdict, mut detail) = match outcome {
        Ok(Ok(())) => (Verdict::Pass, scenario.claim.to_string()),
        Ok(Err(Stop::Fail(m))) => (Verdict::Fail, m),
        Ok(Err(Stop::Unavailable(m))) => (Verdict::Unavailable, m),
        Ok(Err(Stop::Infra(e))) => (Verdict::InfraError, format!("{e:#}")),
        Err(panic) => {
            let message = panic
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| panic.downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_else(|| "panic".into());
            (Verdict::InfraError, format!("runner panic: {message}"))
        }
    };
    if let Err(error) = cleanup {
        ctx.cleanup_failed = true;
        detail = format!("scenario ended {verdict}: {detail}; process cleanup failed: {error:#}");
        verdict = Verdict::InfraError;
    }
    let elapsed = started.elapsed();
    result.verdict = verdict;
    result.detail = detail;
    result.duration_ms = elapsed.as_millis() as u64;
    if verdict != Verdict::InfraError && elapsed > scenario.timeout {
        // A scenario that overran its budget did not hang (its tools are
        // bounded), but its result is still reported; the overrun is evidence.
        result.evidence.insert(
            "overBudgetMs".into(),
            serde_json::json!((elapsed - scenario.timeout).as_millis() as u64),
        );
    }
    result.evidence.extend(std::mem::take(&mut ctx.evidence.0));
    result.artifacts = std::mem::take(&mut ctx.artifacts);
    if verdict == Verdict::Pass && !ctx.keep_media {
        let _ = prune_media(&ctx.scenario_dir);
        result
            .artifacts
            .retain(|reference| ctx.run_dir.join(reference).exists());
    }
    result
}

/// Recordings from a passing scenario are not retained; failures keep them.
fn prune_media(dir: &std::path::Path) -> Result<()> {
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in std::fs::read_dir(&d)?.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().and_then(|e| e.to_str()).is_some_and(|e| {
                matches!(
                    e.to_ascii_lowercase().as_str(),
                    "mkv" | "mp4" | "webm" | "partial" | "msi" | "zip"
                )
            }) {
                let _ = std::fs::remove_file(path);
            }
        }
    }
    Ok(())
}

pub fn run_lane(
    registry: &[Scenario],
    selection: &Selection,
    ctx: &mut Context,
    profile: &str,
) -> LaneResult {
    let started_at = now_rfc3339();
    let mut scenarios = Vec::new();
    for scenario in selected(registry, selection) {
        if ctx.cleanup_failed {
            scenarios.push(ScenarioResult {
                id: scenario.id.into(),
                scenario_revision: scenario.revision,
                verdict: Verdict::Skipped,
                detail: "prior scenario process cleanup failed".into(),
                duration_ms: 0,
                missing_capabilities: vec![],
                evidence: BTreeMap::new(),
                artifacts: vec![],
            });
            continue;
        }
        if selection.skip.iter().any(|s| s == scenario.id) {
            scenarios.push(ScenarioResult {
                id: scenario.id.into(),
                scenario_revision: scenario.revision,
                verdict: Verdict::Skipped,
                detail: "excluded by --skip".into(),
                duration_ms: 0,
                missing_capabilities: vec![],
                evidence: BTreeMap::new(),
                artifacts: vec![],
            });
            continue;
        }
        println!("{:<40} ...", scenario.id);
        let result = run_one(scenario, ctx);
        println!(
            "{:<40} {:<12} {:>7.1}s  {}",
            scenario.id,
            result.verdict,
            result.duration_ms as f64 / 1000.0,
            result.detail
        );
        scenarios.push(result);
    }
    let identity = match &ctx.bundle {
        Some(b) => Identity {
            bundle_sha256: Some(b.sha256.clone()),
            source_commit: Some(b.inventory.source_commit.clone()),
            product_version: Some(b.inventory.product_version.clone()),
            packages: b.inventory.packages(),
        },
        None => Identity::default(),
    };
    let mut tools = BTreeMap::new();
    for name in ["ffprobe", "ffmpeg"] {
        if let Some(path) = crate::tools::resolve(name)
            && let Some(v) = crate::tools::version_line(&path)
        {
            tools.insert(name.to_string(), v);
        }
    }
    LaneResult {
        schema_version: RESULT_SCHEMA_VERSION,
        lane: selection.lane.name().into(),
        profile: profile.into(),
        runner_version: runner_version(),
        started_at,
        finished_at: now_rfc3339(),
        identity,
        environment: ctx.caps.facts.clone(),
        tools,
        capabilities: ctx.caps.names(),
        scenarios,
    }
}
