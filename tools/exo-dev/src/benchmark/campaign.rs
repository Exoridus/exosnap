//! Runs the alternating frontend campaign for one scenario.
//!
//! The order is alternated rather than blocked, so a thermal drift or a
//! background task that builds up over the session lands on both frontends
//! instead of on whichever went second.
//!
//! Every run goes through `benchmark::run::run`, which is the only place that
//! knows how to launch anything. A run that fails its acceptance check stops
//! the campaign: continuing would leave an unbalanced set, and an unbalanced
//! set is worse than a short one.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::Context;

use crate::benchmark::run::{self, Frontend, RunManifest, Scenario};

/// Six runs, alternated. Frontend was removed with the Qt Quick cutover:
/// there is nothing left to alternate against, so the default measures the
/// one frontend that exists, repeatably, rather than defaulting to a
/// frontend this tree cannot build.
pub const DEFAULT_ORDER: [Frontend; 6] = [
    Frontend::Quick,
    Frontend::Quick,
    Frontend::Quick,
    Frontend::Quick,
    Frontend::Quick,
    Frontend::Quick,
];

pub struct CampaignRequest {
    pub scenario: String,
    pub order: Vec<Frontend>,
    pub cooldown: Duration,
    pub output_root: PathBuf,
    pub widgets_exe: Option<PathBuf>,
    pub quick_exe: PathBuf,
    pub superposition_cli: PathBuf,
    pub calibration: bool,
}

pub struct CampaignResult {
    pub runs: Vec<RunManifest>,
}

/// Runs the campaign with the tooling's own default executable/output
/// locations. Callers that need explicit paths (the CLI, a scripted
/// environment) use `run_campaign_with` directly.
pub fn run_campaign(
    scenario: &str,
    order: &[Frontend],
    cooldown: Duration,
) -> anyhow::Result<CampaignResult> {
    let request = CampaignRequest {
        scenario: scenario.to_string(),
        order: order.to_vec(),
        cooldown,
        output_root: default_output_root(),
        widgets_exe: None,
        quick_exe: default_quick_exe(),
        superposition_cli: default_superposition_cli(),
        calibration: false,
    };
    run_campaign_with(&request)
}

pub fn run_campaign_with(request: &CampaignRequest) -> anyhow::Result<CampaignResult> {
    // Relative to the current directory: exo-dev is invoked from the
    // repository root, like every other subcommand's default paths.
    let scenario_path = run::scenario_path(Path::new("."), &request.scenario);
    let definition = run::load_scenario_definition(&scenario_path)
        .with_context(|| format!("unknown scenario '{}'", request.scenario))?;

    let mut counters: HashMap<Frontend, u32> = HashMap::new();
    let mut runs = Vec::new();
    for (index, frontend) in request.order.iter().enumerate() {
        let count = counters.entry(*frontend).or_insert(0);
        *count += 1;
        println!(
            "=== [{}/{}] {frontend} run {count} ===",
            index + 1,
            request.order.len()
        );

        let run_scenario = Scenario {
            definition: definition.clone(),
            frontend: *frontend,
            run_index: *count,
            output_root: request.output_root.clone(),
            widgets_exe: request.widgets_exe.clone(),
            quick_exe: request.quick_exe.clone(),
            superposition_cli: request.superposition_cli.clone(),
            calibration: request.calibration,
            skip_topology_check: false,
        };
        let manifest = crate::benchmark::run::run(&run_scenario)?;
        runs.push(manifest);

        if index + 1 < request.order.len() {
            println!("Cooldown {}s...", request.cooldown.as_secs());
            std::thread::sleep(request.cooldown);
        }
    }

    Ok(CampaignResult { runs })
}

fn default_output_root() -> PathBuf {
    PathBuf::from(".workspace/benchmark-results")
}

fn default_quick_exe() -> PathBuf {
    PathBuf::from("build/windows-x64-release-bench/app/Release/exosnap.exe")
}

fn default_superposition_cli() -> PathBuf {
    PathBuf::from("C:/Program Files/Unigine/Superposition Benchmark/bin/superposition_cli.exe")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_default_order_never_contains_the_removed_widgets_frontend() {
        assert!(
            DEFAULT_ORDER
                .iter()
                .all(|frontend| *frontend != Frontend::Widgets),
            "the widgets frontend no longer exists in the product; the default must not name it"
        );
        assert_eq!(DEFAULT_ORDER.len(), 6);
    }
}
