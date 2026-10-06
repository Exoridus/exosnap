//! `exo-dev encoder-quality-matrix`: sweeps NVENC preset/rate-control
//! combinations through `probe_encode_file`, scores each encode against a
//! reference Y4M clip with an external ffmpeg (libvmaf), and reports
//! BD-rate (bitrate delta at equal quality) across the sweep.
//!
//! This is dev-only tooling: it needs real NVENC hardware and a local
//! ffmpeg build with libvmaf, and nothing here ships in the product. See
//! `docs/dev/encoder-quality-matrix.md` for the full workflow.

mod bd_rate;
mod matrix;
mod process_args;
mod quality;
mod report;
mod sanity;
mod self_test;
mod tuning;
mod y4m;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::Context as _;
use clap::Args;

pub use bd_rate::bd_rate;
pub use matrix::{MatrixCell, RateControl, default_matrix, explicit_matrix};

/// CLI flags, derived one-to-one from the ported script's argparse block:
/// same long names (in kebab-case), same defaults.
#[derive(Args, Debug)]
pub struct MatrixArgs {
    /// Run built-in self-tests and exit.
    #[arg(long)]
    pub self_test: bool,
    /// Score four candidates of known ordering against --clip and verify
    /// the metrics rank them correctly.
    #[arg(long)]
    pub metric_sanity: bool,
    /// Directory for --metric-sanity artifacts. Defaults to a fresh
    /// temporary directory.
    #[arg(long)]
    pub out_dir: Option<PathBuf>,
    /// Reference Y4M clip.
    #[arg(long)]
    pub clip: Option<PathBuf>,
    /// Codec to sweep.
    #[arg(long, value_parser = ["av1", "h264", "hevc"])]
    pub vcodec: Option<String>,
    /// Path to the probe_encode_file executable.
    #[arg(
        long,
        default_value = "build/windows-x64-debug/tools/probes/probe_encode_file/Debug/probe_encode_file.exe"
    )]
    pub probe: PathBuf,
    /// ffmpeg executable with libvmaf support.
    #[arg(long, default_value = "ffmpeg")]
    pub ffmpeg: String,
    /// Output basename (writes .csv and .md).
    #[arg(long, default_value = "quality-matrix-result")]
    pub output: String,
    /// Comma-separated NVENC presets to sweep, e.g. p4,p6,p7. Defaults to
    /// the baseline p4,p7.
    #[arg(long)]
    pub presets: Option<String>,
    /// Comma-separated CQ points, e.g. 16,19,22,24. Defaults to the
    /// baseline 19,24,30,36.
    #[arg(long)]
    pub cq_values: Option<String>,
    /// Comma-separated VBR kbps points. Defaults to the baseline; pass an
    /// empty value to skip VBR.
    #[arg(long)]
    pub vbr_values: Option<String>,
    /// Advanced NVENC tuning, applied identically to every cell in this run.
    #[command(flatten)]
    pub tuning: tuning::TuningArgs,
}

/// One row of the matrix sweep's result: everything `write_report` needs
/// per cell.
#[derive(Debug)]
pub struct MatrixRow {
    pub preset: String,
    pub rc: RateControl,
    pub value: i64,
    pub bitrate_kbps: f64,
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

/// The CLI entry point: `--self-test`, `--metric-sanity`, or the normal
/// sweep, matching the ported script's own `main()` branching.
pub fn run(args: &MatrixArgs) -> anyhow::Result<ExitCode> {
    if args.self_test {
        return Ok(exit_code(self_test::self_test()));
    }

    if args.metric_sanity {
        let clip = args
            .clip
            .as_ref()
            .context("--metric-sanity requires --clip")?;
        let clip = abspath(clip)?;
        let out_dir = match &args.out_dir {
            Some(dir) => abspath(dir)?,
            None => make_temp_dir("exosnap-metric-sanity-")?,
        };
        let passed = sanity::run_metric_sanity(&args.ffmpeg, &clip, &out_dir)?;
        return Ok(exit_code(passed));
    }

    let rows = run_matrix(args)?;
    let clip = abspath(args.clip.as_ref().expect("validated by run_matrix"))?;
    let vcodec = args.vcodec.as_deref().expect("validated by run_matrix");
    report::write_report(&args.output, vcodec, &clip, &rows, &args.ffmpeg)?;
    println!("Wrote {}.csv and {}.md", args.output, args.output);
    Ok(ExitCode::SUCCESS)
}

fn exit_code(passed: bool) -> ExitCode {
    if passed {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

/// Resolves the flags in `args` and runs the full sweep, returning one row
/// per matrix cell. Does not write the report: see [`report::write_report`].
pub fn run_matrix(args: &MatrixArgs) -> anyhow::Result<Vec<MatrixRow>> {
    let clip = args
        .clip
        .as_ref()
        .context("--clip and --vcodec are required unless --self-test is given")?;
    let vcodec = args
        .vcodec
        .as_deref()
        .context("--clip and --vcodec are required unless --self-test is given")?;

    // measure_quality() runs ffmpeg with its working directory set to a
    // scratch directory (see its doc comment for why), so a relative
    // --clip would no longer resolve against the caller's cwd by the time
    // ffmpeg sees it. Resolved once, here, rather than at every clip use
    // site below.
    let clip = abspath(clip)?;
    // Same reason as the clip above: probe_encode() also runs from a
    // scratch cwd.
    let probe = abspath(&args.probe)?;

    process_args::check_ffmpeg_has_libvmaf(&args.ffmpeg)?;
    let cells = resolve_matrix(args)?;

    let (header_line, _) = y4m::read_header_line(&clip)?;
    println!("Reference clip: {}", clip.display());
    println!("Y4M header: {header_line}");

    let clip_name = clip
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("clip")
        .to_string();
    let work_dir = make_temp_dir("exosnap_quality_matrix_")?;
    let ext = match vcodec {
        "av1" => "ivf",
        "hevc" => "h265",
        _ => "h264",
    };

    let mut rows = Vec::new();
    for cell in &cells {
        let out_path = work_dir.join(format!("{clip_name}-{}.{ext}", cell.label()));
        println!("encoding {} ...", cell.label());
        let probe_log = probe_encode(&probe, &clip, &out_path, vcodec, cell, &args.tuning)?;
        std::fs::write(
            work_dir.join(format!("{}.probe.log", cell.label())),
            probe_log,
        )?;
        let bitrate_kbps = std::fs::metadata(&out_path)
            .with_context(|| format!("could not read {}", out_path.display()))?
            .len() as f64
            * 8.0
            / 1000.0
            / y4m::clip_duration_seconds(&clip)?;

        println!("measuring {} ...", cell.label());
        let quality =
            quality::measure_quality(&args.ffmpeg, &out_path, &clip, &work_dir, &cell.label())?;

        rows.push(MatrixRow {
            preset: cell.preset.clone(),
            rc: cell.rc,
            value: cell.value,
            bitrate_kbps,
            vmaf: quality.vmaf,
            vmaf_median: quality.vmaf_median,
            vmaf_p10: quality.vmaf_p10,
            vmaf_p5: quality.vmaf_p5,
            vmaf_p1: quality.vmaf_p1,
            vmaf_worst1_mean: quality.vmaf_worst1_mean,
            vmaf_min: quality.vmaf_min,
            frames: quality.frames,
            libvmaf_version: quality.libvmaf_version,
            ssim: quality.ssim,
            psnr: quality.psnr,
        });
    }
    let metadata = serde_json::json!({
        "codec": vcodec,
        "tuning": args.tuning,
        "reference": clip,
        "reference_sha256": file_sha256(&clip)?,
        "probe": probe,
        "probe_sha256": file_sha256(&probe)?,
        "artifacts": work_dir,
        "encodes": cells.iter().map(|cell| {
            let path = work_dir.join(format!("{clip_name}-{}.{ext}", cell.label()));
            Ok(serde_json::json!({"cell": cell.label(), "path": path, "sha256": file_sha256(&path)?}))
        }).collect::<anyhow::Result<Vec<_>>>()?,
    });
    std::fs::write(
        format!("{}.evidence.json", args.output),
        serde_json::to_vec_pretty(&metadata)?,
    )?;
    Ok(rows)
}

fn file_sha256(path: &Path) -> anyhow::Result<String> {
    use sha2::{Digest, Sha256};
    use std::io::Read;
    let mut file = std::fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut bytes = [0u8; 64 * 1024];
    loop {
        let count = file.read(&mut bytes)?;
        if count == 0 {
            break;
        }
        hash.update(&bytes[..count]);
    }
    Ok(hex::encode(hash.finalize()))
}

fn probe_encode(
    probe: &Path,
    y4m_path: &Path,
    out: &Path,
    vcodec: &str,
    cell: &MatrixCell,
    tuning: &tuning::TuningArgs,
) -> anyhow::Result<String> {
    let mut argv = process_args::probe_encode_argv(
        &probe.to_string_lossy(),
        &y4m_path.to_string_lossy(),
        &out.to_string_lossy(),
        vcodec,
        cell,
    );
    tuning.append_argv(&mut argv);
    let output = std::process::Command::new(&argv[0])
        .args(&argv[1..])
        .output()
        .with_context(|| format!("could not run probe_encode_file for {}", cell.label()))?;
    anyhow::ensure!(
        output.status.success(),
        "probe_encode_file failed for {}:\n{}\n{}",
        cell.label(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(format!(
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    ))
}

/// Sweep selection. When --presets/--cq-values/--vbr-values are all
/// omitted, the baseline matrix runs unchanged, so an existing invocation
/// keeps producing the same cells.
fn resolve_matrix(args: &MatrixArgs) -> anyhow::Result<Vec<MatrixCell>> {
    if args.presets.is_none() && args.cq_values.is_none() && args.vbr_values.is_none() {
        return Ok(matrix::default_matrix());
    }

    let presets: Vec<String> = match &args.presets {
        Some(text) => text
            .split(',')
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .collect(),
        None => vec!["p4".to_string(), "p7".to_string()],
    };
    let cq_values = match &args.cq_values {
        Some(text) => matrix::parse_int_list(text, "--cq-values")?,
        None => vec![19, 24, 30, 36],
    };
    let vbr_values = match &args.vbr_values {
        Some(text) => matrix::parse_int_list(text, "--vbr-values")?,
        None => vec![3000, 6000, 12000, 24000],
    };
    anyhow::ensure!(
        !cq_values.is_empty() || !vbr_values.is_empty(),
        "nothing to sweep: --cq-values and --vbr-values are both empty"
    );
    Ok(matrix::explicit_matrix(&presets, &cq_values, &vbr_values))
}

/// A lexical equivalent of Python's `os.path.abspath`: joins a relative
/// path onto the current directory and collapses `.`/`..` components
/// without touching the filesystem or resolving symlinks. Existence is
/// deliberately not required, matching `os.path.abspath`, since `--probe`
/// and `--clip` are resolved before either is known to exist.
fn abspath(path: &Path) -> anyhow::Result<PathBuf> {
    let joined = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()?.join(path)
    };
    Ok(normalize_lexically(&joined))
}

fn normalize_lexically(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            std::path::Component::ParentDir => {
                out.pop();
            }
            std::path::Component::CurDir => {}
            other => out.push(other),
        }
    }
    out
}

/// A fresh directory under the system temp directory, named the way
/// Python's `tempfile.mkdtemp(prefix=...)` names one. Left in place after
/// the run for later inspection, matching the ported script.
fn make_temp_dir(prefix: &str) -> anyhow::Result<PathBuf> {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("{prefix}{}-{nanos}", std::process::id()));
    std::fs::create_dir_all(&dir).with_context(|| format!("could not create {}", dir.display()))?;
    Ok(dir)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolve_matrix_defaults_to_the_baseline_when_nothing_is_given() {
        let args = MatrixArgs {
            tuning: tuning::TuningArgs::default(),
            self_test: false,
            metric_sanity: false,
            out_dir: None,
            clip: None,
            vcodec: None,
            probe: PathBuf::from("probe"),
            ffmpeg: "ffmpeg".to_string(),
            output: "out".to_string(),
            presets: None,
            cq_values: None,
            vbr_values: None,
        };
        assert_eq!(
            resolve_matrix(&args).unwrap().len(),
            matrix::default_matrix().len()
        );
    }

    #[test]
    fn resolve_matrix_uses_explicit_values_when_any_flag_is_given() {
        let args = MatrixArgs {
            tuning: tuning::TuningArgs::default(),
            self_test: false,
            metric_sanity: false,
            out_dir: None,
            clip: None,
            vcodec: None,
            probe: PathBuf::from("probe"),
            ffmpeg: "ffmpeg".to_string(),
            output: "out".to_string(),
            presets: Some("p1".to_string()),
            cq_values: Some("16,20".to_string()),
            vbr_values: Some(String::new()),
        };
        let cells = resolve_matrix(&args).unwrap();
        let labels: Vec<String> = cells.iter().map(MatrixCell::label).collect();
        assert_eq!(labels, vec!["p1-cq-16", "p1-cq-20"]);
    }

    #[test]
    fn resolve_matrix_rejects_an_entirely_empty_explicit_sweep() {
        let args = MatrixArgs {
            tuning: tuning::TuningArgs::default(),
            self_test: false,
            metric_sanity: false,
            out_dir: None,
            clip: None,
            vcodec: None,
            probe: PathBuf::from("probe"),
            ffmpeg: "ffmpeg".to_string(),
            output: "out".to_string(),
            presets: Some("p1".to_string()),
            cq_values: Some(String::new()),
            vbr_values: Some(String::new()),
        };
        assert!(resolve_matrix(&args).is_err());
    }

    #[test]
    fn abspath_leaves_an_already_absolute_path_unchanged() {
        let absolute = if cfg!(windows) {
            PathBuf::from(r"C:\a\b")
        } else {
            PathBuf::from("/a/b")
        };
        assert_eq!(abspath(&absolute).unwrap(), absolute);
    }

    #[test]
    fn abspath_joins_a_relative_path_onto_the_current_directory() {
        let cwd = std::env::current_dir().unwrap();
        assert_eq!(
            abspath(Path::new("clip.y4m")).unwrap(),
            cwd.join("clip.y4m")
        );
    }

    #[test]
    fn normalize_lexically_collapses_parent_and_current_dir_components() {
        assert_eq!(
            normalize_lexically(Path::new("/a/b/../c/./d")),
            PathBuf::from("/a/c/d")
        );
    }

    #[test]
    fn run_matrix_requires_both_clip_and_vcodec() {
        let args = MatrixArgs {
            tuning: tuning::TuningArgs::default(),
            self_test: false,
            metric_sanity: false,
            out_dir: None,
            clip: None,
            vcodec: None,
            probe: PathBuf::from("probe"),
            ffmpeg: "ffmpeg".to_string(),
            output: "out".to_string(),
            presets: None,
            cq_values: None,
            vbr_values: None,
        };
        let error = run_matrix(&args).unwrap_err();
        assert!(
            error
                .to_string()
                .contains("--clip and --vcodec are required")
        );
    }
}
