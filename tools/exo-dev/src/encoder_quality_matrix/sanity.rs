//! `--metric-sanity`: establishes that the scoring path can tell good from
//! bad before any encoder conclusion is drawn from it.
//!
//! Four candidates are built from the reference itself with an external
//! encoder, so their true ordering is known in advance: a lossless copy, a
//! mildly and a severely degraded encode, and the reference shifted by one
//! frame. VMAF, SSIM and PSNR must all rank them identity > mild > severe,
//! and the one-frame shift must score far below identity. A scoring path
//! that compares the wrong frames, or that scores a colour description
//! instead of pixels, fails at least one of those.
//!
//! No absolute thresholds beyond "clearly separated": the numbers depend on
//! the clip, and a suite that pinned them would turn a content change into
//! a false failure.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, ensure};

use super::process_args;
use super::quality::{self, QualityMeasurement};

fn run_ffmpeg(argv: &[String]) -> anyhow::Result<()> {
    let output = std::process::Command::new(&argv[0])
        .args(&argv[1..])
        .output()
        .with_context(|| format!("could not run ffmpeg ({})", argv.join(" ")))?;
    ensure!(
        output.status.success(),
        "ffmpeg failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(())
}

/// Builds the four sanity candidates, returning `(label, path)` for each.
fn build_sanity_candidates(
    ffmpeg_path: &str,
    clip_path: &Path,
    work_dir: &Path,
) -> anyhow::Result<Vec<(String, PathBuf)>> {
    let mut out = Vec::new();
    let clip = clip_path.to_string_lossy();

    let lossless = work_dir.join("sanity-identity.mkv");
    run_ffmpeg(&process_args::sanity_lossless_argv(
        ffmpeg_path,
        &clip,
        &lossless.to_string_lossy(),
    ))?;
    out.push(("identity".to_string(), lossless));

    for (label, crf) in [("mild", "20"), ("severe", "42")] {
        let path = work_dir.join(format!("sanity-{label}.mkv"));
        run_ffmpeg(&process_args::sanity_libx264_argv(
            ffmpeg_path,
            &clip,
            crf,
            &path.to_string_lossy(),
        ))?;
        out.push((label.to_string(), path));
    }

    let offset = work_dir.join("sanity-offset.mkv");
    run_ffmpeg(&process_args::sanity_offset_argv(
        ffmpeg_path,
        &clip,
        &offset.to_string_lossy(),
    ))?;
    out.push(("offset-1-frame".to_string(), offset));

    Ok(out)
}

fn metric_value(measurement: &QualityMeasurement, metric: &str) -> f64 {
    match metric {
        "vmaf" => measurement.vmaf,
        "ssim" => measurement.ssim.unwrap_or(f64::NAN),
        "psnr" => measurement.psnr.unwrap_or(f64::NAN),
        other => unreachable!("not a metric-sanity metric: {other}"),
    }
}

/// Runs the metric-sanity suite and prints its verdict. Returns whether it
/// passed.
pub fn run_metric_sanity(
    ffmpeg_path: &str,
    clip_path: &Path,
    out_dir: &Path,
) -> anyhow::Result<bool> {
    process_args::check_ffmpeg_has_libvmaf(ffmpeg_path)?;
    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("could not create {}", out_dir.display()))?;

    let mut scores: HashMap<String, QualityMeasurement> = HashMap::new();
    for (label, path) in build_sanity_candidates(ffmpeg_path, clip_path, out_dir)? {
        let measurement = quality::measure_quality(
            ffmpeg_path,
            &path,
            clip_path,
            out_dir,
            &format!("sanity-{label}"),
        )?;
        println!(
            "{label:16} vmaf={:8.4}  p10={:8.4}  worst1%={:8.4}  min={:8.4}  ssim={:.6}  psnr_y={:.3}  frames={}",
            measurement.vmaf,
            measurement.vmaf_p10,
            measurement.vmaf_worst1_mean,
            measurement.vmaf_min,
            measurement.ssim.unwrap_or(f64::NAN),
            measurement.psnr.unwrap_or(f64::NAN),
            measurement.frames,
        );
        scores.insert(label, measurement);
    }

    let mut failures = Vec::new();
    for metric in ["vmaf", "ssim", "psnr"] {
        let identity = metric_value(&scores["identity"], metric);
        let mild = metric_value(&scores["mild"], metric);
        let severe = metric_value(&scores["severe"], metric);
        if !(identity > mild && mild > severe) {
            failures.push(format!(
                "{metric}: identity/mild/severe not strictly ordered: [{identity}, {mild}, {severe}]"
            ));
        }
    }
    for metric in ["vmaf", "ssim", "psnr"] {
        let offset = metric_value(&scores["offset-1-frame"], metric);
        let identity = metric_value(&scores["identity"], metric);
        if offset >= identity {
            failures.push(format!(
                "{metric}: a one-frame offset did not score below identity"
            ));
        }
    }
    if scores["offset-1-frame"].vmaf >= scores["mild"].vmaf {
        failures.push("vmaf: a one-frame offset scored no worse than a mild encode".to_string());
    }

    println!();
    println!(
        "libvmaf: {}, model vmaf_v0.6.1 (libvmaf default)",
        scores["identity"]
            .libvmaf_version
            .as_deref()
            .unwrap_or("unknown")
    );
    println!("artifacts: {}", out_dir.display());
    if !failures.is_empty() {
        for line in &failures {
            println!("FAIL: {line}");
        }
        return Ok(false);
    }
    println!("METRIC SANITY PASSED");
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn measurement(vmaf: f64, ssim: f64, psnr: f64) -> QualityMeasurement {
        QualityMeasurement {
            vmaf,
            vmaf_median: vmaf,
            vmaf_p10: vmaf,
            vmaf_p5: vmaf,
            vmaf_p1: vmaf,
            vmaf_worst1_mean: vmaf,
            vmaf_min: vmaf,
            frames: 10,
            libvmaf_version: Some("2.3.1".to_string()),
            ssim: Some(ssim),
            psnr: Some(psnr),
        }
    }

    #[test]
    fn metric_value_reads_the_named_metric() {
        let m = measurement(90.0, 0.95, 40.0);
        assert_eq!(metric_value(&m, "vmaf"), 90.0);
        assert_eq!(metric_value(&m, "ssim"), 0.95);
        assert_eq!(metric_value(&m, "psnr"), 40.0);
    }

    #[test]
    fn metric_value_falls_back_to_nan_for_a_missing_ssim_or_psnr() {
        let mut m = measurement(90.0, 0.95, 40.0);
        m.ssim = None;
        assert!(metric_value(&m, "ssim").is_nan());
    }
}
