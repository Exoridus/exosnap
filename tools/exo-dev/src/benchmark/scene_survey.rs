//! Surveys candidate Superposition scenes without ExoSnap running.
//!
//! Superposition exposes many built-in scenes and its default benchmark
//! sequence. Which of them is a good ExoSnap workload is an empirical
//! question (continuous motion, high spatial detail, no long static
//! interval, repeatable), so this characterises candidates instead of
//! picking a number out of the manual.
//!
//! ExoSnap is deliberately not running here: this pass characterises the
//! workload alone, which is also the headroom measurement (how much GPU is
//! left for the capture path to use).

use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::Context;

#[derive(Clone, Debug, PartialEq)]
pub struct SuperpositionStats {
    pub frames: usize,
    pub mean_fps: f64,
    pub median_fps: f64,
    pub p1_fps: f64,
    /// Mean absolute frame-to-frame FPS change, in the CSV's own row order. A
    /// scene that sits at a flat rate for most of its runtime has a static
    /// stretch in it, which is the one thing the canonical workload must not
    /// have: a capture path is not stressed by a still image.
    pub fps_variability: f64,
    pub gpu_util_mean: Option<f64>,
    pub gpu_temp_max: Option<f64>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SceneRanking {
    pub label: String,
    pub stats: SuperpositionStats,
}

/// Superposition writes TAB-separated values despite the .csv extension. A
/// comma parse would yield one giant column and every downstream number
/// would silently become empty, so the delimiter is explicit rather than
/// inferred. Low-tail percentiles (not the single worst frame) are used
/// because one hitch during a driver allocation is not a workload
/// characteristic.
pub fn parse_superposition_csv(text: &str) -> anyhow::Result<SuperpositionStats> {
    let mut lines = text.lines();
    let header = lines
        .next()
        .context("empty Superposition CSV: no header row")?;
    let columns: Vec<&str> = header.split('\t').collect();
    let fps_index = columns
        .iter()
        .position(|c| *c == "FPS")
        .context("no FPS column in Superposition CSV")?;
    let util_index = columns.iter().position(|c| c.contains("UTILIZATION"));
    let temp_index = columns.iter().position(|c| c.contains("TEMPERATURE"));

    let mut fps = Vec::new();
    let mut util = Vec::new();
    let mut temp = Vec::new();
    for line in lines {
        if line.trim().is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split('\t').collect();
        if let Some(value) = fields
            .get(fps_index)
            .and_then(|v| v.trim().parse::<f64>().ok())
            && value > 0.0
        {
            fps.push(value);
        }
        if let Some(index) = util_index
            && let Some(value) = fields.get(index).and_then(|v| v.trim().parse::<f64>().ok())
            && value >= 0.0
        {
            util.push(value);
        }
        if let Some(index) = temp_index
            && let Some(value) = fields.get(index).and_then(|v| v.trim().parse::<f64>().ok())
            && value > 0.0
        {
            temp.push(value);
        }
    }
    anyhow::ensure!(
        !fps.is_empty(),
        "no usable FPS samples in Superposition CSV"
    );

    let variability = if fps.len() > 1 {
        let mut sum = 0.0;
        for window in fps.windows(2) {
            sum += (window[1] - window[0]).abs();
        }
        sum / (fps.len() - 1) as f64
    } else {
        0.0
    };

    let mut sorted = fps.clone();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let mean = fps.iter().sum::<f64>() / fps.len() as f64;

    Ok(SuperpositionStats {
        frames: fps.len(),
        mean_fps: mean,
        median_fps: percentile(&sorted, 0.50),
        p1_fps: percentile(&sorted, 0.01),
        fps_variability: variability,
        gpu_util_mean: mean_of(&util),
        gpu_temp_max: max_of(&temp),
    })
}

fn percentile(sorted_ascending: &[f64], fraction: f64) -> f64 {
    let index = ((fraction * (sorted_ascending.len() - 1) as f64).floor() as usize)
        .min(sorted_ascending.len() - 1);
    sorted_ascending[index]
}

fn mean_of(values: &[f64]) -> Option<f64> {
    if values.is_empty() {
        None
    } else {
        Some(values.iter().sum::<f64>() / values.len() as f64)
    }
}

fn max_of(values: &[f64]) -> Option<f64> {
    values.iter().cloned().fold(None, |max, v| match max {
        None => Some(v),
        Some(m) if v > m => Some(v),
        Some(m) => Some(m),
    })
}

/// The scene identifier a candidate directory's own name encodes: `scene-4`
/// runs Superposition scene 4, anything else runs its default sequence.
fn scene_mode_args(label: &str) -> Vec<String> {
    match label.strip_prefix("scene-") {
        Some(number) => vec!["-mode".to_string(), "scene".to_string(), number.to_string()],
        None => vec!["-mode".to_string(), "default".to_string()],
    }
}

fn run_superposition(
    cli: &Path,
    mode_args: &[String],
    csv: &Path,
    txt: &Path,
) -> anyhow::Result<()> {
    let mut args = vec![
        "-api".to_string(),
        "directx".to_string(),
        "-resolution".to_string(),
        "2560x1440".to_string(),
        "-fullscreen".to_string(),
        "2".to_string(),
        "-quality".to_string(),
        "high".to_string(),
        "-textures".to_string(),
        "high".to_string(),
        "-dof".to_string(),
        "0".to_string(),
        "-motion_blur".to_string(),
        "0".to_string(),
        "-sound".to_string(),
        "0".to_string(),
        "-iterations".to_string(),
        "1".to_string(),
    ];
    args.extend(mode_args.iter().cloned());
    args.push("-mode_duration".to_string());
    args.push("1".to_string());
    args.push("-log_csv".to_string());
    args.push(csv.display().to_string());
    args.push("-log_csv_step".to_string());
    args.push("0".to_string());
    args.push("-log_txt".to_string());
    args.push(txt.display().to_string());

    let status = Command::new(cli)
        .args(&args)
        .status()
        .with_context(|| format!("could not start {}", cli.display()))?;
    anyhow::ensure!(
        csv.is_file(),
        "{}: no CSV produced (exit {status})",
        cli.display()
    );
    Ok(())
}

/// Runs every candidate scene through the external Superposition CLI without
/// ExoSnap running, and ranks them by FPS variability (highest first): a
/// scene on continuous motion and detail is picked on that basis, never on
/// its raw score.
///
/// Each entry in `candidate_scenes` is the output directory for one
/// candidate; its final path component is the scene label (`scene-4`, or
/// anything else for the default sequence), and the CSV/TXT reports are
/// written inside it.
pub fn survey(
    candidate_scenes: &[PathBuf],
    superposition_cli: &Path,
) -> anyhow::Result<Vec<SceneRanking>> {
    anyhow::ensure!(
        superposition_cli.is_file(),
        "Superposition CLI not found at {}.",
        superposition_cli.display()
    );

    let mut rankings = Vec::new();
    for dir in candidate_scenes {
        std::fs::create_dir_all(dir)
            .with_context(|| format!("could not create {}", dir.display()))?;
        let label = dir
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("scene")
            .to_string();
        let csv = dir.join(format!("{label}.csv"));
        let txt = dir.join(format!("{label}.txt"));
        run_superposition(superposition_cli, &scene_mode_args(&label), &csv, &txt)?;

        let text = std::fs::read_to_string(&csv)
            .with_context(|| format!("could not read {}", csv.display()))?;
        let stats = parse_superposition_csv(&text)
            .with_context(|| format!("{label}: could not parse Superposition CSV"))?;
        rankings.push(SceneRanking { label, stats });
    }

    rankings.sort_by(|a, b| {
        b.stats
            .fps_variability
            .partial_cmp(&a.stats.fps_variability)
            .unwrap()
    });
    Ok(rankings)
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE_CSV: &str = "FPS\tGPU0 UTILIZATION [%]\tGPU0 TEMPERATURE [C]\n60.0\t50\t60\n120.0\t90\t70\n30.0\t95\t75\n";

    #[test]
    fn tab_separated_fps_and_gpu_columns_are_parsed_into_the_expected_statistics() {
        let stats = parse_superposition_csv(SAMPLE_CSV).unwrap();
        assert_eq!(stats.frames, 3);
        assert!((stats.mean_fps - 70.0).abs() < 1e-9);
        assert!((stats.median_fps - 60.0).abs() < 1e-9);
        assert!((stats.p1_fps - 30.0).abs() < 1e-9);
        // |120-60| + |30-120| over the two consecutive gaps, averaged: (60+90)/2.
        assert!((stats.fps_variability - 75.0).abs() < 1e-9);
        assert!((stats.gpu_util_mean.unwrap() - 78.333_333_333).abs() < 1e-6);
        assert_eq!(stats.gpu_temp_max, Some(75.0));
    }

    #[test]
    fn a_comma_separated_file_is_rejected_for_lacking_an_fps_column() {
        let error = parse_superposition_csv("FPS,UTIL\n60,50\n").unwrap_err();
        assert!(error.to_string().contains("FPS"), "{error}");
    }

    #[test]
    fn zero_and_negative_fps_rows_are_dropped_before_computing_statistics() {
        let csv = "FPS\n60.0\n0\n-5\n120.0\n";
        let stats = parse_superposition_csv(csv).unwrap();
        assert_eq!(stats.frames, 2);
    }

    #[test]
    fn a_single_sample_has_zero_variability_and_no_gpu_columns_are_optional() {
        let stats = parse_superposition_csv("FPS\n60.0\n").unwrap();
        assert_eq!(stats.frames, 1);
        assert_eq!(stats.fps_variability, 0.0);
        assert_eq!(stats.gpu_util_mean, None);
        assert_eq!(stats.gpu_temp_max, None);
    }

    #[test]
    fn an_empty_csv_is_an_error_not_a_default() {
        assert!(parse_superposition_csv("").is_err());
    }

    #[test]
    fn scene_mode_args_read_the_scene_number_from_the_label() {
        assert_eq!(scene_mode_args("scene-4"), vec!["-mode", "scene", "4"]);
        assert_eq!(
            scene_mode_args("default-sequence"),
            vec!["-mode", "default"]
        );
    }

    #[test]
    fn surveying_without_a_real_superposition_cli_fails_with_a_clear_message() {
        let dir = tempfile::tempdir().unwrap();
        let candidates = vec![dir.path().join("scene-4")];
        let error = survey(
            &candidates,
            Path::new("Z:/does-not-exist/superposition_cli.exe"),
        )
        .unwrap_err();
        assert!(
            error.to_string().contains("Superposition CLI not found"),
            "{error}"
        );
    }
}
