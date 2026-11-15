//! JSON and Markdown rendering of a toolchain run, plus ninja's own per-edge
//! timing, which needs no profiler and works for every compiler.

use std::fmt::Write as _;
use std::path::Path;

use anyhow::Result;
use serde::Serialize;

use super::{Sample, Skipped, ToolInfo};

#[derive(Serialize)]
pub struct RunReport {
    pub revision: String,
    pub target: String,
    pub jobs: usize,
    pub reps: usize,
    pub baseline: String,
    pub tools: Vec<ToolInfo>,
    /// (variant, scenario, median wall seconds, peak commit bytes)
    pub summary: Vec<(String, String, f64, Option<u64>)>,
    pub samples: Vec<Sample>,
    pub skipped: Vec<Skipped>,
}

#[derive(Serialize)]
pub struct Edge {
    pub output: String,
    pub seconds: f64,
}

/// The slowest outputs of the most recent build of each output, read from
/// `.ninja_log` (tab separated: start ms, end ms, mtime, output, command hash).
pub fn slowest_edges(build_dir: &Path, count: usize) -> Option<Vec<Edge>> {
    let text = std::fs::read_to_string(build_dir.join(".ninja_log")).ok()?;
    let mut latest: std::collections::HashMap<&str, f64> = std::collections::HashMap::new();
    for line in text.lines().skip(1) {
        let fields: Vec<&str> = line.split('\t').collect();
        if let [start, end, _, output, ..] = fields[..] {
            if let (Ok(start), Ok(end)) = (start.parse::<f64>(), end.parse::<f64>()) {
                latest.insert(output, (end - start) / 1000.0);
            }
        }
    }
    let mut edges: Vec<Edge> = latest
        .into_iter()
        .map(|(output, seconds)| Edge {
            output: output.to_string(),
            seconds,
        })
        .collect();
    edges.sort_by(|a, b| b.seconds.total_cmp(&a.seconds));
    edges.truncate(count);
    Some(edges)
}

fn mib(bytes: u64) -> String {
    format!("{:.0} MiB", bytes as f64 / (1024.0 * 1024.0))
}

fn delta(value: f64, baseline: Option<f64>) -> String {
    match baseline {
        Some(base) if base > 0.0 => format!("{:+.1}%", (value / base - 1.0) * 100.0),
        _ => "-".to_string(),
    }
}

pub fn write_run_report(out: &Path, report: &RunReport) -> Result<()> {
    std::fs::write(
        out.join("report.json"),
        serde_json::to_string_pretty(report)?,
    )?;
    std::fs::write(out.join("report.md"), render_markdown(report))?;
    Ok(())
}

pub fn render_markdown(report: &RunReport) -> String {
    let mut md = String::new();
    let _ = writeln!(md, "# Toolchain run\n");
    let _ = writeln!(
        md,
        "Revision `{}`, target `{}`, {} jobs, {} repetitions, baseline `{}`.\n",
        report.revision, report.target, report.jobs, report.reps, report.baseline
    );
    let _ = writeln!(md, "## Tools\n\n| Tool | Version |\n|---|---|");
    for tool in &report.tools {
        let _ = writeln!(
            md,
            "| {} | {} |",
            tool.name,
            tool.version.as_deref().unwrap_or("not found")
        );
    }
    let mut scenarios: Vec<&str> = Vec::new();
    for (_, scenario, _, _) in &report.summary {
        if !scenarios.contains(&scenario.as_str()) {
            scenarios.push(scenario);
        }
    }
    for scenario in scenarios {
        let _ = writeln!(
            md,
            "\n## {scenario}\n\nMedian wall seconds. The delta is relative to the baseline.\n\n| Variant | Wall | Delta | Peak commit |\n|---|---|---|---|"
        );
        let baseline = report
            .summary
            .iter()
            .find(|(variant, s, _, _)| *variant == report.baseline && s == scenario)
            .map(|(_, _, wall, _)| *wall);
        for (variant, s, wall, peak) in &report.summary {
            if s != scenario {
                continue;
            }
            let _ = writeln!(
                md,
                "| {variant} | {wall:.1} s | {} | {} |",
                delta(*wall, baseline),
                peak.map_or("unavailable".to_string(), mib)
            );
        }
    }
    let failed: Vec<&Sample> = report
        .samples
        .iter()
        .filter(|s| s.timing.exit_code != 0)
        .collect();
    if !failed.is_empty() {
        let _ = writeln!(md, "\n## Failed samples\n");
        for sample in failed {
            let _ = writeln!(
                md,
                "- {} {} #{}: exit {}, log `{}`",
                sample.variant,
                sample.scenario,
                sample.rep,
                sample.timing.exit_code,
                sample.log.display()
            );
        }
    }
    let cached: Vec<&Sample> = report
        .samples
        .iter()
        .filter(|s| s.sccache.is_some())
        .collect();
    if !cached.is_empty() {
        let _ = writeln!(
            md,
            "\n## sccache\n\n| Variant | Scenario | Rep | Requests | Hits | Misses | Cache size |\n|---|---|---|---|---|---|---|"
        );
        for sample in cached {
            let stats = sample.sccache.as_ref().expect("filtered on Some");
            let _ = writeln!(
                md,
                "| {} | {} | {} | {} | {} | {} | {} |",
                sample.variant,
                sample.scenario,
                sample.rep,
                stats.requests_executed,
                stats.hits,
                stats.misses,
                mib(stats.cache_dir_bytes)
            );
        }
    }
    if !report.skipped.is_empty() {
        let _ = writeln!(md, "\n## Skipped\n");
        for skipped in &report.skipped {
            let _ = writeln!(md, "- {}: {}", skipped.variant, skipped.reason);
        }
    }
    md
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delta_is_relative_to_the_baseline() {
        assert_eq!(delta(90.0, Some(100.0)), "-10.0%");
        assert_eq!(delta(90.0, None), "-");
    }

    #[test]
    fn slowest_edges_keep_the_latest_entry_per_output() {
        let dir = std::env::temp_dir().join(format!("toolbench-ninjalog-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join(".ninja_log"),
            "# ninja log v5\n0\t5000\t0\ta.obj\th1\n0\t1000\t0\tb.obj\th2\n0\t2000\t0\ta.obj\th3\n",
        )
        .unwrap();
        let edges = slowest_edges(&dir, 5).unwrap();
        assert_eq!(edges[0].output, "a.obj");
        assert_eq!(edges[0].seconds, 2.0);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
