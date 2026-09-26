//! Builds the cross-run comparison dataset for a set of benchmark runs.
//!
//! Two rules do the real work here:
//!
//!   * A set is only compared when every accepted run shares the same
//!     `effective_recording_config.fingerprint`. Matching command lines are
//!     not evidence; the fingerprint is read back from what the engine was
//!     handed, and a mismatch is a hard error, never a silent skip.
//!
//!   * A delta is only computed for a metric whose comparability the report
//!     schema itself marks `identical`. An `approximate` metric is printed
//!     side by side with its probe text; a `frontend_only` metric is never
//!     subtracted at all. A Widgets preview "frame" was a swap-chain Present
//!     of one quad; a Quick preview "frame" is a scene-graph render of the
//!     whole window. Subtracting two structurally different things produces
//!     a number that looks like an answer and is not one.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::Context;
use serde_json::Value;

use crate::benchmark::scene_survey::{self, SuperpositionStats};

#[derive(Clone, Debug)]
pub struct RunSummary {
    pub run_id: String,
    pub frontend: String,
}

#[derive(Clone, Debug)]
pub struct ExcludedRun {
    pub run_id: String,
    pub reasons: Vec<String>,
}

#[derive(Clone, Debug, Default)]
pub struct Stats {
    pub mean: f64,
    pub min: f64,
    pub max: f64,
    /// Spread relative to the mean, as a percentage. A delta smaller than
    /// the run-to-run spread is not a finding, and the report has to make
    /// that visible instead of implying precision the sample size does not
    /// support.
    pub spread_percent: f64,
    pub n: usize,
}

fn stats(values: &[f64]) -> Option<Stats> {
    if values.is_empty() {
        return None;
    }
    let mean = values.iter().sum::<f64>() / values.len() as f64;
    let min = values.iter().cloned().fold(f64::INFINITY, f64::min);
    let max = values.iter().cloned().fold(f64::NEG_INFINITY, f64::max);
    let spread = if mean != 0.0 {
        (max - min) / mean.abs() * 100.0
    } else {
        0.0
    };
    Some(Stats {
        mean,
        min,
        max,
        spread_percent: spread,
        n: values.len(),
    })
}

#[derive(Clone, Debug)]
pub struct MetricRow {
    pub metric: String,
    pub comparability: Option<String>,
    pub groups: BTreeMap<String, Stats>,
    pub probes: BTreeMap<String, String>,
    pub delta: Option<f64>,
    pub delta_percent: Option<f64>,
}

#[derive(Clone, Debug)]
pub struct WorkloadRow {
    pub run_id: String,
    pub frontend: String,
    pub stats: SuperpositionStats,
}

#[derive(Clone, Debug)]
pub struct ComparisonReport {
    pub fingerprint: String,
    pub accepted_runs: Vec<RunSummary>,
    pub excluded_runs: Vec<ExcludedRun>,
    pub metrics: Vec<MetricRow>,
    pub superposition_workload: Vec<WorkloadRow>,
}

struct LoadedRun {
    run_id: String,
    frontend: String,
    directory: PathBuf,
    report: Value,
    fingerprint: Option<String>,
    accepted: bool,
    rejections: Vec<String>,
}

fn load_run(dir: &Path) -> anyhow::Result<Option<LoadedRun>> {
    let manifest_path = dir.join("run.json");
    if !manifest_path.is_file() {
        return Ok(None);
    }
    let manifest: Value = serde_json::from_str(
        &std::fs::read_to_string(&manifest_path)
            .with_context(|| format!("could not read {}", manifest_path.display()))?,
    )
    .with_context(|| format!("could not parse {}", manifest_path.display()))?;

    let run_id = manifest["run_id"].as_str().unwrap_or_default().to_string();
    let frontend = manifest["frontend"]
        .as_str()
        .unwrap_or_default()
        .to_string();
    let Some(report_name) = manifest["exosnap_report"].as_str() else {
        anyhow::bail!("{run_id}: no ExoSnap report recorded in run.json");
    };
    let report_path = dir.join(report_name);
    let report: Value = serde_json::from_str(
        &std::fs::read_to_string(&report_path)
            .with_context(|| format!("could not read {}", report_path.display()))?,
    )
    .with_context(|| format!("could not parse {}", report_path.display()))?;

    let mut reasons = Vec::new();
    if manifest["calibration"].as_bool().unwrap_or(false) {
        reasons.push("calibration run".to_string());
    }
    if !manifest["topology_verified"].as_bool().unwrap_or(true) {
        reasons.push("topology unverified".to_string());
    }
    let exit_code = manifest["exosnap_exit_code"].as_i64().unwrap_or(-1);
    if exit_code != 0 {
        reasons.push(format!("exit code {exit_code}"));
    }
    if !report["outcome"]["succeeded"].as_bool().unwrap_or(false) {
        reasons.push("recording did not succeed".to_string());
    }
    let build_config = report["environment"]["build_config"].as_str().unwrap_or("");
    if build_config != "Release" {
        reasons.push(format!(
            "build config {build_config} (accepted runs must be Release)"
        ));
    }
    let fingerprint_available = report["effective_recording_config"]["available"]
        .as_bool()
        .unwrap_or(false);
    if !fingerprint_available {
        reasons.push("no effective config fingerprint".to_string());
    }

    let fingerprint = fingerprint_available
        .then(|| {
            report["effective_recording_config"]["fingerprint"]
                .as_str()
                .map(str::to_string)
        })
        .flatten();

    Ok(Some(LoadedRun {
        run_id,
        frontend,
        directory: dir.to_path_buf(),
        report,
        fingerprint,
        accepted: reasons.is_empty(),
        rejections: reasons,
    }))
}

fn metric_value<'a>(report: &'a Value, name: &str) -> Option<&'a Value> {
    let (group, key) = name.split_once('.')?;
    report[group][key].as_object().map(|_| &report[group][key])
}

fn metric_names(report: &Value) -> Vec<String> {
    let mut names = Vec::new();
    for group in ["preview", "recording", "process"] {
        let Some(object) = report[group].as_object() else {
            continue;
        };
        for key in object.keys() {
            names.push(format!("{group}.{key}"));
        }
    }
    names
}

/// Compares a set of runs. Fails (never silently skips) when accepted runs
/// disagree on `effective_recording_config.fingerprint`, since that fact is
/// what makes the rest of the comparison meaningful at all.
pub fn compare(run_dirs: &[PathBuf]) -> anyhow::Result<ComparisonReport> {
    let mut runs = Vec::new();
    for dir in run_dirs {
        if let Some(run) = load_run(dir)? {
            runs.push(run);
        }
    }

    let accepted: Vec<&LoadedRun> = runs.iter().filter(|r| r.accepted).collect();

    let mut fingerprints: Vec<&str> = accepted
        .iter()
        .filter_map(|r| r.fingerprint.as_deref())
        .collect();
    fingerprints.sort_unstable();
    fingerprints.dedup();
    anyhow::ensure!(
        fingerprints.len() <= 1,
        "effective recording configurations diverge across accepted runs ({}); refusing to \
         produce a comparison, the runs did not record the same thing",
        fingerprints.join(", ")
    );
    let fingerprint = fingerprints.first().unwrap_or(&"").to_string();

    let mut metric_order: Vec<String> = Vec::new();
    for run in &accepted {
        for name in metric_names(&run.report) {
            if !metric_order.contains(&name) {
                metric_order.push(name);
            }
        }
    }

    let mut rows = Vec::new();
    for name in &metric_order {
        let mut comparability = None;
        let mut groups: BTreeMap<String, Vec<f64>> = BTreeMap::new();
        let mut probes: BTreeMap<String, String> = BTreeMap::new();

        for run in &accepted {
            let Some(metric) = metric_value(&run.report, name) else {
                continue;
            };
            if comparability.is_none() {
                comparability = metric["comparability"].as_str().map(str::to_string);
            }
            if let Some(value) = metric["value"].as_f64() {
                groups.entry(run.frontend.clone()).or_default().push(value);
                if let Some(probe) = metric["probe"].as_str()
                    && !probes.contains_key(&run.frontend)
                {
                    probes.insert(run.frontend.clone(), probe.to_string());
                }
            }
        }

        let group_stats: BTreeMap<String, Stats> = groups
            .iter()
            .filter_map(|(frontend, values)| stats(values).map(|s| (frontend.clone(), s)))
            .collect();

        let (delta, delta_percent) =
            if comparability.as_deref() == Some("identical") && group_stats.len() == 2 {
                let mut iter = group_stats.iter();
                let (_, a) = iter.next().unwrap();
                let (_, b) = iter.next().unwrap();
                let delta = b.mean - a.mean;
                let delta_percent = if a.mean != 0.0 {
                    Some(delta / a.mean.abs() * 100.0)
                } else {
                    None
                };
                (Some(delta), delta_percent)
            } else {
                (None, None)
            };

        rows.push(MetricRow {
            metric: name.clone(),
            comparability,
            groups: group_stats,
            probes,
            delta,
            delta_percent,
        });
    }

    let mut workload = Vec::new();
    for run in &accepted {
        let csv_path = run.directory.join("superposition.csv");
        let Ok(text) = std::fs::read_to_string(&csv_path) else {
            continue;
        };
        if let Ok(stats) = scene_survey::parse_superposition_csv(&text) {
            workload.push(WorkloadRow {
                run_id: run.run_id.clone(),
                frontend: run.frontend.clone(),
                stats,
            });
        }
    }

    Ok(ComparisonReport {
        fingerprint,
        accepted_runs: accepted
            .iter()
            .map(|r| RunSummary {
                run_id: r.run_id.clone(),
                frontend: r.frontend.clone(),
            })
            .collect(),
        excluded_runs: runs
            .iter()
            .filter(|r| !r.accepted)
            .map(|r| ExcludedRun {
                run_id: r.run_id.clone(),
                reasons: r.rejections.clone(),
            })
            .collect(),
        metrics: rows,
        superposition_workload: workload,
    })
}

pub fn render_table(report: &ComparisonReport) -> String {
    let mut out = String::new();
    out.push_str(&format!(
        "Accepted: {} run(s). Effective config fingerprint: {}\n",
        report.accepted_runs.len(),
        report.fingerprint
    ));
    for row in &report.metrics {
        out.push_str(&format!(
            "{:<32} {:<12} ",
            row.metric,
            row.comparability.as_deref().unwrap_or("")
        ));
        for (frontend, stats) in &row.groups {
            out.push_str(&format!("{frontend}={:.3} ", stats.mean));
        }
        if let Some(delta) = row.delta {
            out.push_str(&format!("delta={delta:.3}"));
            if let Some(percent) = row.delta_percent {
                out.push_str(&format!(" ({percent:.1}%)"));
            }
        }
        out.push('\n');
    }
    out
}

/// Writes the machine-readable dataset. The shape mirrors `ComparisonReport`
/// closely enough that a reader does not need this module to make sense of
/// it.
pub fn write_json(report: &ComparisonReport, path: &Path) -> anyhow::Result<()> {
    let document = serde_json::json!({
        "effective_config_hash": report.fingerprint,
        "accepted_runs": report.accepted_runs.iter().map(|r| serde_json::json!({
            "run_id": r.run_id,
            "frontend": r.frontend,
        })).collect::<Vec<_>>(),
        "excluded_runs": report.excluded_runs.iter().map(|r| serde_json::json!({
            "run_id": r.run_id,
            "reasons": r.reasons,
        })).collect::<Vec<_>>(),
        "metrics": report.metrics.iter().map(|row| serde_json::json!({
            "metric": row.metric,
            "comparability": row.comparability,
            "groups": row.groups.iter().map(|(frontend, s)| (frontend.clone(), serde_json::json!({
                "mean": s.mean, "min": s.min, "max": s.max,
                "spread_percent": s.spread_percent, "n": s.n,
            }))).collect::<BTreeMap<_, _>>(),
            "probes": row.probes,
            "delta": row.delta,
            "delta_percent": row.delta_percent,
        })).collect::<Vec<_>>(),
        "superposition_workload": report.superposition_workload.iter().map(|w| serde_json::json!({
            "run_id": w.run_id,
            "frontend": w.frontend,
            "frames": w.stats.frames,
            "mean_fps": w.stats.mean_fps,
            "median_fps": w.stats.median_fps,
            "p1_fps": w.stats.p1_fps,
            "fps_variability": w.stats.fps_variability,
            "gpu_util_mean": w.stats.gpu_util_mean,
            "gpu_temp_max": w.stats.gpu_temp_max,
        })).collect::<Vec<_>>(),
    });
    std::fs::write(path, serde_json::to_string_pretty(&document)?)
        .with_context(|| format!("could not write {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_run(
        dir: &Path,
        run_id: &str,
        frontend: &str,
        fingerprint: &str,
        metrics: &[(&str, f64, &str)],
    ) {
        std::fs::create_dir_all(dir).unwrap();
        let manifest = serde_json::json!({
            "run_id": run_id,
            "frontend": frontend,
            "calibration": false,
            "topology_verified": true,
            "exosnap_exit_code": 0,
            "exosnap_report": "report.json",
        });
        std::fs::write(
            dir.join("run.json"),
            serde_json::to_string(&manifest).unwrap(),
        )
        .unwrap();

        let mut preview = serde_json::Map::new();
        for (name, value, comparability) in metrics {
            let key = name.strip_prefix("preview.").unwrap_or(name);
            preview.insert(
                key.to_string(),
                serde_json::json!({ "value": value, "comparability": comparability, "probe": "p" }),
            );
        }
        let report = serde_json::json!({
            "outcome": { "succeeded": true },
            "environment": { "build_config": "Release" },
            "effective_recording_config": { "available": true, "fingerprint": fingerprint },
            "preview": preview,
        });
        std::fs::write(
            dir.join("report.json"),
            serde_json::to_string(&report).unwrap(),
        )
        .unwrap();
    }

    #[test]
    fn a_fingerprint_mismatch_between_accepted_runs_is_a_hard_error_not_a_silent_skip() {
        let root = tempfile::tempdir().unwrap();
        let a = root.path().join("widgets-run01");
        let b = root.path().join("quick-run01");
        write_run(
            &a,
            "widgets-run01",
            "widgets",
            "fp-a",
            &[("preview.frame_time_ms", 10.0, "identical")],
        );
        write_run(
            &b,
            "quick-run01",
            "quick",
            "fp-b",
            &[("preview.frame_time_ms", 8.0, "identical")],
        );

        let error = compare(&[a, b]).unwrap_err();
        assert!(error.to_string().contains("diverge"), "{error}");
    }

    #[test]
    fn a_delta_is_computed_only_for_identical_comparability_metrics() {
        let root = tempfile::tempdir().unwrap();
        let a = root.path().join("widgets-run01");
        let b = root.path().join("quick-run01");
        write_run(
            &a,
            "widgets-run01",
            "widgets",
            "fp",
            &[
                ("preview.frame_time_ms", 10.0, "identical"),
                ("preview.present_kind", 1.0, "frontend_only"),
                ("preview.probe_point", 3.0, "approximate"),
            ],
        );
        write_run(
            &b,
            "quick-run01",
            "quick",
            "fp",
            &[
                ("preview.frame_time_ms", 8.0, "identical"),
                ("preview.present_kind", 2.0, "frontend_only"),
                ("preview.probe_point", 5.0, "approximate"),
            ],
        );

        let report = compare(&[a, b]).unwrap();
        let identical_row = report
            .metrics
            .iter()
            .find(|r| r.metric == "preview.frame_time_ms")
            .unwrap();
        // Two groups, ordered alphabetically by frontend name ("quick" then
        // "widgets"): the delta is the second group's mean minus the first's.
        assert_eq!(identical_row.delta, Some(2.0));

        let frontend_only_row = report
            .metrics
            .iter()
            .find(|r| r.metric == "preview.present_kind")
            .unwrap();
        assert_eq!(frontend_only_row.delta, None);

        let approximate_row = report
            .metrics
            .iter()
            .find(|r| r.metric == "preview.probe_point")
            .unwrap();
        assert_eq!(approximate_row.delta, None);
    }

    #[test]
    fn a_run_missing_its_report_is_rejected_rather_than_crashing_the_comparison() {
        let root = tempfile::tempdir().unwrap();
        let dir = root.path().join("quick-run01");
        std::fs::create_dir_all(&dir).unwrap();
        let manifest = serde_json::json!({
            "run_id": "quick-run01",
            "frontend": "quick",
            "calibration": false,
            "topology_verified": true,
            "exosnap_exit_code": 0,
            "exosnap_report": null,
        });
        std::fs::write(
            dir.join("run.json"),
            serde_json::to_string(&manifest).unwrap(),
        )
        .unwrap();

        let error = compare(&[dir]).unwrap_err();
        assert!(error.to_string().contains("no ExoSnap report"), "{error}");
    }

    #[test]
    fn a_directory_without_a_manifest_is_silently_absent_not_a_fatal_error() {
        let root = tempfile::tempdir().unwrap();
        let dir = root.path().join("not-a-run");
        std::fs::create_dir_all(&dir).unwrap();
        let report = compare(&[dir]).unwrap();
        assert!(report.accepted_runs.is_empty());
    }
}
