//! Scoring one encode against the reference clip through ffmpeg's libvmaf
//! filter, which also reports SSIM/PSNR when asked.

use std::path::Path;

use anyhow::{Context as _, ensure};
use serde_json::Value;

use super::process_args;

/// Pooled and per-frame quality figures for one scored encode.
///
/// A high mean VMAF can hide a handful of bad scroll or scene-change
/// frames, so the tail (median, p10, p5, p1, worst-1%-mean, minimum) is
/// reported alongside it.
pub struct QualityMeasurement {
    pub vmaf: f64,
    pub vmaf_median: f64,
    pub vmaf_p10: f64,
    pub vmaf_p5: f64,
    pub vmaf_p1: f64,
    pub vmaf_worst1_mean: f64,
    pub vmaf_min: f64,
    pub frames: usize,
    pub libvmaf_version: Option<String>,
    pub ssim: Option<f64>,
    pub psnr: Option<f64>,
}

/// Runs ffmpeg's libvmaf filter comparing `distorted_path` (the probe's
/// encode) against the original `reference_path` Y4M, and parses the
/// resulting JSON log.
///
/// `log_dir` becomes the child's working directory, matching
/// [`process_args::measure_quality_filter_arg`]'s bare-filename log path.
pub fn measure_quality(
    ffmpeg_path: &str,
    distorted_path: &Path,
    reference_path: &Path,
    log_dir: &Path,
    label: &str,
) -> anyhow::Result<QualityMeasurement> {
    let vmaf_log_name = format!("{label}.vmaf.json");
    let vmaf_log_path = log_dir.join(&vmaf_log_name);
    let filter_arg = process_args::measure_quality_filter_arg(&vmaf_log_name);
    let argv = process_args::measure_quality_argv(
        ffmpeg_path,
        &distorted_path.to_string_lossy(),
        &reference_path.to_string_lossy(),
        &filter_arg,
    );

    let output = std::process::Command::new(&argv[0])
        .args(&argv[1..])
        .current_dir(log_dir)
        .output()
        .with_context(|| format!("could not run ffmpeg for {label}"))?;
    ensure!(
        output.status.success(),
        "ffmpeg quality measurement failed for {label}:\n{}",
        String::from_utf8_lossy(&output.stderr)
    );

    let json_text = std::fs::read_to_string(&vmaf_log_path)
        .with_context(|| format!("could not read {}", vmaf_log_path.display()))?;
    parse_vmaf_report(&json_text)
}

/// Parses a libvmaf JSON log (produced by ffmpeg's libvmaf filter with
/// `log_fmt=json`) into pooled and per-frame metrics.
pub fn parse_vmaf_report(json_text: &str) -> anyhow::Result<QualityMeasurement> {
    let report: Value =
        serde_json::from_str(json_text).context("could not parse libvmaf JSON log")?;
    let pooled = report
        .get("pooled_metrics")
        .context("libvmaf JSON log has no pooled_metrics")?;
    let frames_value = report
        .get("frames")
        .and_then(Value::as_array)
        .context("libvmaf JSON log has no frames array")?;

    let mut per_frame: Vec<f64> = frames_value
        .iter()
        .map(|frame| {
            frame
                .get("metrics")
                .and_then(|metrics| metrics.get("vmaf"))
                .and_then(Value::as_f64)
                .context("a frame entry is missing metrics.vmaf")
        })
        .collect::<anyhow::Result<Vec<f64>>>()?;
    ensure!(!per_frame.is_empty(), "libvmaf JSON log has no frames");
    per_frame.sort_by(|a, b| a.partial_cmp(b).unwrap());

    // Mean of the worst 1% of frames, at least one frame wide. On screen
    // content the upper percentiles saturate at exactly 100 and only the
    // extreme tail moves, but a single frame's minimum is easy to move by
    // one outlier; averaging the worst percentile keeps that tail readable
    // without resting a verdict on one frame.
    let worst_n = (per_frame.len() / 100).max(1);
    let vmaf_worst1_mean = per_frame[..worst_n].iter().sum::<f64>() / worst_n as f64;

    // libvmaf's `feature=name=psnr` reports per-plane psnr_y/psnr_cb/psnr_cr
    // in pooled_metrics; there is no combined "psnr" key. psnr_y (luma
    // PSNR) is the conventional scalar for encoder quality reporting.
    let pooled_mean = |key: &str| {
        pooled
            .get(key)
            .and_then(|entry| entry.get("mean"))
            .and_then(Value::as_f64)
    };

    Ok(QualityMeasurement {
        vmaf: pooled_mean("vmaf").context("pooled_metrics has no vmaf.mean")?,
        vmaf_median: median(&per_frame),
        vmaf_p10: percentile(&per_frame, 10.0),
        vmaf_p5: percentile(&per_frame, 5.0),
        vmaf_p1: percentile(&per_frame, 1.0),
        vmaf_worst1_mean,
        vmaf_min: per_frame[0],
        frames: per_frame.len(),
        libvmaf_version: report.get("version").map(value_as_display_string),
        ssim: pooled_mean("float_ssim"),
        psnr: pooled_mean("psnr_y"),
    })
}

fn value_as_display_string(value: &Value) -> String {
    match value {
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

fn median(sorted: &[f64]) -> f64 {
    let n = sorted.len();
    if n % 2 == 1 {
        sorted[n / 2]
    } else {
        (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0
    }
}

fn percentile(sorted: &[f64], pct: f64) -> f64 {
    let n = sorted.len();
    let idx = round_half_even(pct / 100.0 * (n - 1) as f64) as i64;
    let idx = idx.clamp(0, n as i64 - 1) as usize;
    sorted[idx]
}

/// Python's `round()` uses round-half-to-even, not round-half-away-from-zero
/// (`f64::round`). VMAF percentile frame selection needs the same tie-break
/// as the ported script so identical inputs land on the same frame index.
fn round_half_even(x: f64) -> f64 {
    let floor = x.floor();
    let diff = x - floor;
    if diff < 0.5 {
        floor
    } else if diff > 0.5 {
        floor + 1.0
    } else if (floor as i64) % 2 == 0 {
        floor
    } else {
        floor + 1.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn synthetic_report(vmaf_values: &[f64]) -> String {
        let frames: Vec<String> = vmaf_values
            .iter()
            .map(|v| format!(r#"{{"metrics":{{"vmaf":{v}}}}}"#))
            .collect();
        format!(
            r#"{{"version":"2.3.1","frames":[{}],"pooled_metrics":{{"vmaf":{{"mean":90.0}},"float_ssim":{{"mean":0.98}},"psnr_y":{{"mean":40.0}}}}}}"#,
            frames.join(",")
        )
    }

    #[test]
    fn parses_pooled_and_per_frame_metrics() {
        let json = synthetic_report(&[80.0, 85.0, 90.0, 95.0, 100.0]);
        let measurement = parse_vmaf_report(&json).unwrap();
        assert_eq!(measurement.vmaf, 90.0);
        assert_eq!(measurement.vmaf_min, 80.0);
        assert_eq!(measurement.vmaf_median, 90.0);
        assert_eq!(measurement.frames, 5);
        assert_eq!(measurement.libvmaf_version.as_deref(), Some("2.3.1"));
        assert_eq!(measurement.ssim, Some(0.98));
        assert_eq!(measurement.psnr, Some(40.0));
    }

    #[test]
    fn worst1_mean_is_at_least_one_frame_wide() {
        // 5 frames: 1% of 5 rounds down to 0, so the worst-1% mean must
        // still cover exactly the single worst frame, not zero frames.
        let json = synthetic_report(&[10.0, 50.0, 60.0, 70.0, 80.0]);
        let measurement = parse_vmaf_report(&json).unwrap();
        assert_eq!(measurement.vmaf_worst1_mean, 10.0);
    }

    #[test]
    fn round_half_even_matches_pythons_round() {
        // Python: round(0.5) == 0, round(1.5) == 2, round(2.5) == 2.
        assert_eq!(round_half_even(0.5), 0.0);
        assert_eq!(round_half_even(1.5), 2.0);
        assert_eq!(round_half_even(2.5), 2.0);
        assert_eq!(round_half_even(0.4), 0.0);
        assert_eq!(round_half_even(0.6), 1.0);
    }

    #[test]
    fn a_report_missing_pooled_vmaf_is_an_error() {
        let json = r#"{"frames":[{"metrics":{"vmaf":90.0}}],"pooled_metrics":{}}"#;
        assert!(parse_vmaf_report(json).is_err());
    }

    #[test]
    fn a_report_with_no_frames_is_an_error() {
        let json = r#"{"frames":[],"pooled_metrics":{"vmaf":{"mean":90.0}}}"#;
        assert!(parse_vmaf_report(json).is_err());
    }
}
