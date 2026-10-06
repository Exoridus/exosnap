//! Resumable P4 advanced-tuning qualification using the matrix scoring path.

use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

use anyhow::{Context as _, ensure};
use clap::Args;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use super::{
    MatrixCell, RateControl, file_sha256, probe_encode, quality, sanity, tuning::TuningArgs, y4m,
};

const SCHEMA: u32 = 1;

#[derive(Args, Debug)]
pub struct CampaignArgs {
    /// Frozen local inputs and their provenance, in JSON.
    #[arg(long)]
    pub manifest: PathBuf,
    /// Local evidence root. Reuse it to resume the exact same campaign.
    #[arg(long)]
    pub output: PathBuf,
}

#[derive(Deserialize, Serialize)]
struct Manifest {
    probe: PathBuf,
    build_receipt: PathBuf,
    ffmpeg: PathBuf,
    ffprobe: PathBuf,
    nvenc_sdk: String,
    libvmaf: String,
    model: String,
    clips: Vec<Clip>,
    #[serde(default = "reserve")]
    reserve_gib: u64,
    archive: Option<PathBuf>,
    editor_probe: Option<PathBuf>,
}

fn reserve() -> u64 {
    60
}

#[derive(Deserialize, Serialize)]
struct Clip {
    name: String,
    class: String,
    path: PathBuf,
    provenance: String,
    source_sha256: String,
    source_path: Option<PathBuf>,
    production: String,
    qualified: bool,
}

fn points(rc: &str) -> [i64; 4] {
    if rc == "cq" {
        [19, 24, 30, 36]
    } else {
        [3000, 6000, 12000, 24000]
    }
}

fn median(values: &[f64]) -> f64 {
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let middle = sorted.len() / 2;
    if sorted.len().is_multiple_of(2) {
        (sorted[middle - 1] + sorted[middle]) / 2.0
    } else {
        sorted[middle]
    }
}

fn quality_gate(clips: &[f64]) -> bool {
    clips.len() == 3 && clips.iter().all(|v| v.is_finite() && *v <= 2.0) && median(clips) <= -5.0
}

fn reusable(saved: &Value, identity: &Value) -> bool {
    saved["identity"] == *identity && saved["status"] == "COMPLETED"
}

fn variants(rc: &str) -> Vec<(&'static str, TuningArgs)> {
    let names: &[&str] = if rc == "cq" {
        &["BASE", "B", "L", "T", "SAQ", "BL", "BLT"]
    } else {
        &["BASE", "SAQ", "B", "L", "T", "QMP", "FMP", "COMBINED"]
    };
    names
        .iter()
        .map(|&name| {
            let b = matches!(name, "B" | "BL" | "BLT" | "COMBINED");
            (
                name,
                TuningArgs {
                    bframes: if b { 2 } else { 0 },
                    b_ref: if b { "middle" } else { "off" }.into(),
                    lookahead: matches!(name, "L" | "BL" | "BLT" | "COMBINED"),
                    lookahead_depth: 16,
                    spatial_aq: name == "SAQ",
                    temporal_aq: matches!(name, "T" | "BLT" | "COMBINED"),
                    multipass: match name {
                        "QMP" => "quarter",
                        "FMP" | "COMBINED" => "full",
                        _ => "single",
                    }
                    .into(),
                },
            )
        })
        .collect()
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("system clock")
        .as_secs()
}

fn write_json(path: &Path, value: &Value) -> anyhow::Result<()> {
    let pending = path.with_extension("json.pending");
    std::fs::write(&pending, serde_json::to_vec_pretty(value)?)?;
    std::fs::rename(&pending, path)?;
    Ok(())
}

fn capture(exe: &Path, args: &[&str]) -> anyhow::Result<String> {
    let output = Command::new(exe)
        .args(args)
        .output()
        .with_context(|| format!("start {}", exe.display()))?;
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    ensure!(output.status.success(), "{} failed: {text}", exe.display());
    Ok(text)
}

fn field(line: &str, name: &str) -> Option<f64> {
    line.split_whitespace().find_map(|token| {
        token
            .strip_prefix(&format!("{name}="))
            .and_then(|v| v.trim_end_matches("ms").parse().ok())
    })
}

fn timing(log: &str, prefix: &str, name: &str) -> Option<f64> {
    log.lines()
        .find(|line| line.contains(prefix))
        .and_then(|line| field(line, name))
}

fn resolved_matches(log: &str, tuning: &TuningArgs) -> bool {
    let Some(line) = log.lines().find(|line| line.contains("RESOLVED ")) else {
        return false;
    };
    let has = |name: &str, value: &str| {
        line.split_whitespace()
            .any(|token| token == format!("{name}={value}"))
    };
    has("bframes", &tuning.bframes.to_string())
        && has("b_ref", &tuning.b_ref)
        && has(
            "lookahead",
            &if tuning.lookahead {
                tuning.lookahead_depth
            } else {
                0
            }
            .to_string(),
        )
        && has("spatial_aq", if tuning.spatial_aq { "1" } else { "0" })
        && has("temporal_aq", if tuning.temporal_aq { "1" } else { "0" })
        && has("multipass", &tuning.multipass)
}

fn unsupported(caps: &str, codec: &str, tuning: &TuningArgs) -> anyhow::Result<Option<String>> {
    let lines: Vec<_> = caps
        .lines()
        .filter(|line| line.contains(&format!("CAPS codec={codec} ")))
        .collect();
    ensure!(
        lines.len() == 1,
        "campaign requires one unambiguous adapter capability record per codec"
    );
    let line = lines[0];
    let read = |name| field(line, name).with_context(|| format!("missing capability {name}"));
    let reason = if read("supported")? == 0.0 {
        Some("codec unsupported")
    } else if read("max_bframes")? < tuning.bframes as f64 {
        Some("B2 unsupported")
    } else if tuning.bframes > 0 && (read("b_ref")? as u32 & 2) == 0 {
        Some("Middle B-reference unsupported")
    } else if tuning.lookahead && read("lookahead")? == 0.0 {
        Some("Lookahead unsupported")
    } else if tuning.temporal_aq && read("temporal_aq")? == 0.0 {
        Some("Temporal AQ unsupported")
    } else {
        None
    };
    Ok(reason.map(str::to_owned))
}

#[cfg(windows)]
mod host {
    use super::*;
    use std::os::windows::ffi::OsStrExt;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetDiskFreeSpaceExW(
            path: *const u16,
            available: *mut u64,
            total: *mut u64,
            free: *mut u64,
        ) -> i32;
        fn SetThreadExecutionState(flags: u32) -> u32;
    }

    pub fn free(path: &Path) -> anyhow::Result<u64> {
        let path: Vec<u16> = path.as_os_str().encode_wide().chain([0]).collect();
        let mut available = 0;
        // The nul-terminated path and all output pointers live through the call.
        ensure!(
            unsafe {
                GetDiskFreeSpaceExW(
                    path.as_ptr(),
                    &mut available,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            } != 0,
            "disk free-space query failed"
        );
        Ok(available)
    }

    pub struct Awake(u32);
    impl Awake {
        pub fn new() -> anyhow::Result<Self> {
            // Only this runner's thread requests system wakefulness, never display wakefulness.
            let previous = unsafe { SetThreadExecutionState(0x80000001) };
            ensure!(previous != 0, "execution-state request failed");
            Ok(Self(previous))
        }
    }
    impl Drop for Awake {
        fn drop(&mut self) {
            // Restore this thread's prior continuous request even on an early return.
            unsafe {
                SetThreadExecutionState(self.0 | 0x80000000);
            }
        }
    }
}

#[cfg(not(windows))]
mod host {
    use super::*;
    pub fn free(_: &Path) -> anyhow::Result<u64> {
        anyhow::bail!("campaign requires Windows")
    }
    pub struct Awake;
    impl Awake {
        pub fn new() -> anyhow::Result<Self> {
            anyhow::bail!("campaign requires Windows")
        }
    }
}

fn measure(
    manifest: &Manifest,
    clip: &Clip,
    codec: &str,
    rc: &str,
    point: i64,
    tuning: &TuningArgs,
    dir: &Path,
) -> anyhow::Result<Value> {
    let cell = MatrixCell {
        preset: "p4".into(),
        rc: if rc == "cq" {
            RateControl::Cq
        } else {
            RateControl::Vbr
        },
        value: point,
    };
    let media = dir.join(match codec {
        "av1" => "encoded.ivf",
        "hevc" => "encoded.h265",
        _ => "encoded.h264",
    });
    let mut argv = super::process_args::probe_encode_argv(
        &manifest.probe.to_string_lossy(),
        &clip.path.to_string_lossy(),
        &media.to_string_lossy(),
        codec,
        &cell,
    );
    tuning.append_argv(&mut argv);
    write_json(
        &dir.join("commands.json"),
        &json!({"encode": argv, "scoring_cwd": dir, "scoring": super::process_args::measure_quality_argv(&manifest.ffmpeg.to_string_lossy(), &media.to_string_lossy(), &clip.path.to_string_lossy(), &super::process_args::measure_quality_filter_arg("score.vmaf.json"))}),
    )?;
    let mut attempt = 0;
    let log = loop {
        match probe_encode(&manifest.probe, &clip.path, &media, codec, &cell, tuning) {
            Ok(log) => break log,
            Err(error) => {
                std::fs::write(
                    dir.join(format!("encode-attempt-{attempt}.log")),
                    format!("{error:#}"),
                )?;
                let text = format!("{error:#}");
                if attempt == 0
                    && (text.contains("DEVICE_NOT_EXIST") || text.contains("device lost"))
                {
                    attempt += 1;
                } else {
                    return Err(error);
                }
            }
        }
    };
    std::fs::write(dir.join("probe.log"), &log)?;
    ensure!(
        resolved_matches(&log, tuning),
        "resolved tuning differs from requested tuning"
    );
    ensure!(
        timing(&log, "BACKLOG", "after_flush") == Some(0.0),
        "encoder did not drain"
    );
    let score = quality::measure_quality(
        &manifest.ffmpeg.to_string_lossy(),
        &media,
        &clip.path,
        dir,
        "score",
    )?;
    ensure!(
        score.libvmaf_version.as_deref() == Some(manifest.libvmaf.as_str()),
        "libvmaf identity differs from manifest"
    );
    let duration = y4m::clip_duration_seconds(&clip.path)?;
    ensure!(
        (score.frames as f64 - duration * 60.0).abs() < 0.5,
        "scored frame count differs from reference"
    );
    let hash = file_sha256(&media)?;
    let bitrate = std::fs::metadata(&media)?.len() as f64 * 8.0 / duration / 1000.0;
    Ok(
        json!({"bitrate_kbps": bitrate, "vmaf": score.vmaf, "vmaf_median": score.vmaf_median,
        "vmaf_p10": score.vmaf_p10, "vmaf_p5": score.vmaf_p5, "vmaf_p1": score.vmaf_p1,
        "vmaf_worst1_mean": score.vmaf_worst1_mean, "vmaf_min": score.vmaf_min,
        "ssim": score.ssim, "psnr": score.psnr, "frames": score.frames, "libvmaf": score.libvmaf_version,
        "encoded_sha256": hash, "encoded_path": media, "raw_retained": true,
        "service_p50_ms": timing(&log, "TIMING", "p50"), "service_p99_ms": timing(&log, "TIMING", "p99"),
        "service_peak_ms": timing(&log, "TIMING", "max"), "residency_p50_ms": timing(&log, "ENCODER_LATENCY", "p50"),
        "residency_p99_ms": timing(&log, "ENCODER_LATENCY", "p99"), "residency_peak_ms": timing(&log, "ENCODER_LATENCY", "max"),
        "peak_backlog": timing(&log, "BACKLOG", "peak"), "slots": timing(&log, "RESOLVED", "slots"),
        "output_depth": timing(&log, "RESOLVED", "output_depth"), "attempts": attempt + 1}),
    )
}

fn cell_run(
    manifest: &Manifest,
    environment: &Value,
    clip: &Clip,
    spec: (&str, &str, &str, i64, &TuningArgs),
    root: &Path,
) -> anyhow::Result<Value> {
    let (phase, codec, rc, point, tuning) = spec;
    let variant = variants(rc)
        .into_iter()
        .find(|(_, candidate)| {
            serde_json::to_value(candidate).ok() == serde_json::to_value(tuning).ok()
        })
        .map(|(name, _)| name)
        .context("unknown tuning")?;
    let key = format!("{phase}-{}-{codec}-{rc}-{point}-{variant}", clip.name);
    let dir = root.join("cells").join(&key);
    std::fs::create_dir_all(&dir)?;
    let identity = json!({"environment": environment, "clip": clip.name, "codec": codec, "rc": rc, "preset": "p4", "variant": variant, "tuning": tuning, "value": point, "phase": phase});
    let result_path = dir.join("result.json");
    if result_path.exists() {
        let saved: Value = serde_json::from_slice(&std::fs::read(&result_path)?)?;
        ensure!(
            saved["identity"] == identity,
            "cell identity differs; use a new campaign root: {key}"
        );
        if reusable(&saved, &identity) {
            let media = saved["metrics"]["encoded_path"]
                .as_str()
                .context("missing encoded path")?;
            ensure!(
                file_sha256(Path::new(media))? == saved["metrics"]["encoded_sha256"],
                "completed media hash differs: {key}"
            );
            println!("RESUME {key}");
            let mut saved = saved;
            saved["resumed"] = json!(true);
            return Ok(saved);
        }
        let previous = dir.join(format!("previous-{}.json", now()));
        std::fs::copy(&result_path, previous)?;
    }
    let started = now();
    println!("RUN {key}");
    let result = if let Some(reason) = unsupported(
        environment["capabilities"]
            .as_str()
            .context("missing caps")?,
        codec,
        tuning,
    )? {
        json!({"status": "N/A", "reason": reason})
    } else {
        ensure!(
            host::free(root)? > manifest.reserve_gib * 1024 * 1024 * 1024 + 2 * 1024 * 1024 * 1024,
            "disk reserve reached; completed cells preserved"
        );
        let attempt_dir = dir.join(format!("attempt-{}", now()));
        std::fs::create_dir_all(&attempt_dir)?;
        match measure(manifest, clip, codec, rc, point, tuning, &attempt_dir) {
            Ok(metrics) => json!({"status": "COMPLETED", "metrics": metrics}),
            Err(error) => {
                let reason = format!("{error:#}");
                std::fs::write(dir.join("failure.log"), &reason)?;
                json!({"status": "FAILED", "reason": reason})
            }
        }
    };
    let mut result = result;
    result["identity"] = identity;
    result["key"] = json!(key);
    result["started"] = json!(started);
    result["finished"] = json!(now());
    result["resumed"] = json!(false);
    write_json(&result_path, &result)?;
    Ok(result)
}

fn curve<'a>(
    cells: &'a [Value],
    phase: &str,
    clip: &str,
    codec: &str,
    rc: &str,
    variant: &str,
) -> Option<Vec<&'a Value>> {
    points(rc)
        .iter()
        .map(|point| {
            cells.iter().find(|cell| {
                let id = &cell["identity"];
                cell["status"] == "COMPLETED"
                    && id["phase"] == phase
                    && id["clip"] == clip
                    && id["codec"] == codec
                    && id["rc"] == rc
                    && id["variant"] == variant
                    && id["value"] == *point
            })
        })
        .collect()
}

fn comparisons(manifest: &Manifest, cells: &[Value], phase: &str) -> Vec<Value> {
    let mut results = Vec::new();
    for codec in ["h264", "hevc", "av1"] {
        for rc in ["cq", "vbr"] {
            for (variant, _) in variants(rc).into_iter().filter(|(name, _)| *name != "BASE") {
                let mut per_clip = serde_json::Map::new();
                let mut rates = Vec::new();
                let mut errors = Vec::new();
                let mut service = true;
                for clip in &manifest.clips {
                    let pair = curve(cells, phase, &clip.name, codec, rc, "BASE")
                        .zip(curve(cells, phase, &clip.name, codec, rc, variant));
                    let result = pair
                        .context("incomplete curve")
                        .and_then(|(base, candidate)| {
                            service &= candidate.iter().all(|c| {
                                c["metrics"]["service_p99_ms"]
                                    .as_f64()
                                    .is_some_and(|v| v < 8.0)
                            });
                            let values = |rows: &[&Value], key: &str| -> anyhow::Result<Vec<f64>> {
                                rows.iter()
                                    .map(|row| {
                                        row["metrics"][key]
                                            .as_f64()
                                            .context("missing finite metric")
                                    })
                                    .collect()
                            };
                            super::bd_rate(
                                &values(&base, "bitrate_kbps")?,
                                &values(&base, "vmaf")?,
                                &values(&candidate, "bitrate_kbps")?,
                                &values(&candidate, "vmaf")?,
                            )
                        });
                    match result {
                        Ok(rate) => {
                            rates.push(rate);
                            per_clip.insert(clip.name.clone(), json!(rate));
                        }
                        Err(error) => {
                            per_clip.insert(clip.name.clone(), Value::Null);
                            errors.push(format!("{}: {error:#}", clip.name));
                        }
                    }
                }
                let classes: std::collections::BTreeSet<_> = manifest
                    .clips
                    .iter()
                    .filter(|c| c.qualified)
                    .map(|c| c.class.as_str())
                    .collect();
                let references = classes
                    == std::collections::BTreeSet::from(["gameplay", "stable", "desktop"])
                    && manifest.clips.len() == 3;
                let complete = rates.len() == manifest.clips.len() && errors.is_empty();
                let quality = complete && quality_gate(&rates);
                let verdict = if !complete || !references {
                    "BLOCKED"
                } else if quality && service {
                    "QUALITY_AND_SERVICE_PASS"
                } else {
                    "FAIL"
                };
                results.push(json!({"phase": phase, "codec": codec, "rc": rc, "variant": variant, "per_clip": per_clip,
                    "median": if complete && rates.len() == 3 { Some(median(&rates)) } else { None }, "best": rates.iter().copied().reduce(f64::min), "worst": rates.iter().copied().reduce(f64::max), "valid_clip_count": rates.len(),
                    "quality_pass": quality, "service_pass": service && complete, "references_qualified": references, "verdict": verdict, "errors": errors,
                    "service_screen_ms": 8.0, "default_changed": false, "compatibility_complete": false}));
            }
        }
    }
    results
}

fn csv_field(value: &Value) -> String {
    let text = value.as_str().map(str::to_owned).unwrap_or_else(|| {
        if value.is_null() {
            String::new()
        } else {
            value.to_string()
        }
    });
    format!("\"{}\"", text.replace('"', "\"\""))
}

fn compatibility(
    manifest: &Manifest,
    codec: &str,
    rc: &str,
    name: &str,
    root: &Path,
) -> anyhow::Result<Value> {
    let dir = root
        .join("compatibility")
        .join(format!("{codec}-{rc}-{name}"));
    std::fs::create_dir_all(&dir)?;
    let tuning = variants(rc)
        .into_iter()
        .find(|(v, _)| *v == name)
        .context("winner tuning")?
        .1;
    let cell = MatrixCell {
        preset: "p4".into(),
        rc: if rc == "cq" {
            RateControl::Cq
        } else {
            RateControl::Vbr
        },
        value: if rc == "cq" { 24 } else { 6000 },
    };
    let elementary = dir.join(if codec == "av1" {
        "sample.ivf"
    } else if codec == "hevc" {
        "sample.h265"
    } else {
        "sample.h264"
    });
    let mkv = dir.join("sample.mkv");
    let mp4 = dir.join("sample.mp4");
    let mut argv = super::process_args::probe_encode_argv(
        &manifest.probe.to_string_lossy(),
        &manifest.clips[0].path.to_string_lossy(),
        &elementary.to_string_lossy(),
        codec,
        &cell,
    );
    tuning.append_argv(&mut argv);
    argv.extend(["--mkv-output".into(), mkv.to_string_lossy().into_owned()]);
    if codec != "av1" {
        argv.extend(["--mp4-output".into(), mp4.to_string_lossy().into_owned()]);
    }
    let args: Vec<_> = argv[1..].iter().map(String::as_str).collect();
    let encoded = capture(&manifest.probe, &args)?;
    std::fs::write(dir.join("probe.log"), &encoded)?;
    ensure!(
        resolved_matches(&encoded, &tuning),
        "winner tuning mismatch"
    );
    let mut files = Vec::new();
    for media in [&mkv, &mp4].into_iter().filter(|p| p.exists()) {
        let path = media.to_str().context("media path encoding")?;
        let decoded = capture(
            &manifest.ffmpeg,
            &["-v", "error", "-xerror", "-i", path, "-f", "null", "-"],
        )?;
        std::fs::write(media.with_extension("decode.log"), decoded)?;
        let packets = capture(
            &manifest.ffprobe,
            &[
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_packets",
                "-show_streams",
                "-of",
                "json",
                path,
            ],
        )?;
        std::fs::write(media.with_extension("ffprobe.json"), &packets)?;
        let data: Value = serde_json::from_str(&packets)?;
        let frames = data["packets"].as_array().context("missing packets")?;
        ensure!(!frames.is_empty(), "empty winner container");
        let mut previous = None;
        for packet in frames {
            ensure!(
                packet["pts_time"]
                    .as_str()
                    .and_then(|v| v.parse::<f64>().ok())
                    .is_some(),
                "missing winner PTS"
            );
            if let Some(dts) = packet["dts_time"]
                .as_str()
                .and_then(|v| v.parse::<f64>().ok())
            {
                ensure!(
                    previous.is_none_or(|last| dts >= last),
                    "nonmonotonic winner DTS"
                );
                previous = Some(dts);
            }
        }
        let seek = capture(
            &manifest.ffmpeg,
            &[
                "-v",
                "error",
                "-xerror",
                "-ss",
                "3",
                "-i",
                path,
                "-frames:v",
                "1",
                "-f",
                "framemd5",
                "-",
            ],
        )?;
        ensure!(
            seek.lines()
                .any(|line| !line.starts_with('#') && line.contains(',')),
            "seek decoded no frame"
        );
        std::fs::write(media.with_extension("seek.log"), seek)?;
        files.push(json!({"path": media, "sha256": file_sha256(media)?, "full_decode": true, "timestamps": true, "seek_command_passed": true}));
    }
    let editor = manifest
        .editor_probe
        .as_ref()
        .context("headless editor probe unavailable; compatibility incomplete")?;
    let editor_log = capture(
        editor,
        &[mkv.to_str().context("MKV path")?, "0", "--decode-only"],
    )?;
    std::fs::write(dir.join("editor.log"), &editor_log)?;
    ensure!(
        timing(
            &editor_log,
            "[B] video_frames_delivered=",
            "video_frames_delivered"
        )
        .is_some_and(|frames| frames > 0.0),
        "editor produced no frames"
    );
    Ok(
        json!({"files": files, "editor_probe": editor, "editor_probe_sha256": file_sha256(editor)?, "editor_log": dir.join("editor.log")}),
    )
}

fn reports(
    root: &Path,
    manifest: &Manifest,
    cells: &[Value],
    initial: &[Value],
    confirmation: &[Value],
) -> anyhow::Result<()> {
    let mut raw = "key,status,phase,clip,codec,rc,variant,value,bitrate_kbps,vmaf,vmaf_median,vmaf_p10,vmaf_p5,vmaf_p1,vmaf_worst1_mean,vmaf_min,ssim,psnr,service_p50_ms,service_p99_ms,service_peak_ms,residency_p50_ms,residency_p99_ms,residency_peak_ms,peak_backlog,slots,output_depth,reason\n".to_owned();
    let mut failures = "# Failed and unsupported cells\n\n".to_owned();
    for cell in cells {
        let mut columns = vec![&cell["key"], &cell["status"]];
        for key in ["phase", "clip", "codec", "rc", "variant", "value"] {
            columns.push(&cell["identity"][key]);
        }
        for key in [
            "bitrate_kbps",
            "vmaf",
            "vmaf_median",
            "vmaf_p10",
            "vmaf_p5",
            "vmaf_p1",
            "vmaf_worst1_mean",
            "vmaf_min",
            "ssim",
            "psnr",
            "service_p50_ms",
            "service_p99_ms",
            "service_peak_ms",
            "residency_p50_ms",
            "residency_p99_ms",
            "residency_peak_ms",
            "peak_backlog",
            "slots",
            "output_depth",
        ] {
            columns.push(&cell["metrics"][key]);
        }
        columns.push(&cell["reason"]);
        raw.push_str(
            &columns
                .into_iter()
                .map(csv_field)
                .collect::<Vec<_>>()
                .join(","),
        );
        raw.push('\n');
        if cell["status"] != "COMPLETED" {
            failures.push_str(&format!(
                "- {}: {} {}\n",
                cell["key"], cell["status"], cell["reason"]
            ));
        }
    }
    std::fs::write(root.join("cells.csv"), raw)?;
    let mut bd =
        "phase,codec,rc,variant,clip,bd_rate_percent,median,best,worst,verdict\n".to_owned();
    let mut summary = "# P4 NVENC campaign\n\nNo product defaults changed. Negative BD-rate means bitrate saving at equal mean VMAF. Per-frame tails remain in cells.csv and score.vmaf.json.\n\nThe documented probe TIMING is service cost including waits. ENCODER_LATENCY is output residency, including intentional reordering/lookahead. The 8 ms service screen reserves over half of a 60 fps frame interval; it is a conservative campaign screen, not a newly approved project threshold. Live capture timing, physical A/V sync, visual inspection and editor decode are not established by this campaign.\n\n".to_owned();
    let mut curves = "phase,clip,codec,rc,variant,complete,points,measurements\n".to_owned();
    for phase in ["main", "confirmation"] {
        for clip in &manifest.clips {
            for codec in ["h264", "hevc", "av1"] {
                for rc in ["cq", "vbr"] {
                    for (variant, _) in variants(rc) {
                        let rows = curve(cells, phase, &clip.name, codec, rc, variant);
                        curves.push_str(&format!(
                            "{phase},{},{codec},{rc},{variant},{},\"{:?}\",{}\n",
                            clip.name,
                            rows.is_some(),
                            points(rc),
                            csv_field(&json!(rows.map(|r| {
                                r.into_iter()
                                    .map(|c| c["metrics"].clone())
                                    .collect::<Vec<_>>()
                            })))
                        ));
                    }
                }
            }
        }
    }
    for result in initial.iter().chain(confirmation) {
        if let Some(errors) = result["errors"].as_array() {
            for error in errors {
                failures.push_str(&format!(
                    "- BD-rate {} {} {} {}: {}\n",
                    result["phase"], result["codec"], result["rc"], result["variant"], error
                ));
            }
        }
        if !result["compatibility_error"].is_null() {
            failures.push_str(&format!(
                "- Winner compatibility: {}\n",
                result["compatibility_error"]
            ));
        }
        for clip in &manifest.clips {
            let columns = [
                &result["phase"],
                &result["codec"],
                &result["rc"],
                &result["variant"],
                &json!(clip.name),
                &result["per_clip"][&clip.name],
                &result["median"],
                &result["best"],
                &result["worst"],
                &result["verdict"],
            ];
            bd.push_str(
                &columns
                    .into_iter()
                    .map(csv_field)
                    .collect::<Vec<_>>()
                    .join(","),
            );
            bd.push('\n');
        }
    }
    for rc in ["cq", "vbr"] {
        for codec in ["h264", "hevc", "av1"] {
            summary.push_str(&format!("## {codec} {rc}\n\n| Variant |"));
            for clip in &manifest.clips {
                summary.push_str(&format!(" {} |", clip.name));
            }
            summary.push_str(" Median | Gate |\n|---|");
            for _ in &manifest.clips {
                summary.push_str("---:|");
            }
            summary.push_str("---:|---|\n");
            for result in initial
                .iter()
                .filter(|r| r["codec"] == codec && r["rc"] == rc)
            {
                summary.push_str(&format!(
                    "| {} |",
                    result["variant"].as_str().unwrap_or("?")
                ));
                for clip in &manifest.clips {
                    summary.push_str(&format!(" {} |", result["per_clip"][&clip.name]));
                }
                summary.push_str(&format!(
                    " {} | {} |\n",
                    result["median"],
                    result["verdict"].as_str().unwrap_or("?")
                ));
            }
            summary.push('\n');
        }
    }
    write_json(
        &root.join("qualification.json"),
        &json!({"initial": initial, "confirmation": confirmation, "default_changes": false}),
    )?;
    std::fs::write(root.join("curves.csv"), curves)?;
    std::fs::write(root.join("bd-rate.csv"), bd)?;
    std::fs::write(root.join("summary.md"), summary)?;
    std::fs::write(root.join("failures.md"), failures)?;
    Ok(())
}

fn archive(root: &Path, destination: &Path) -> anyhow::Result<()> {
    std::fs::create_dir_all(destination)?;
    let mut hashes = Vec::new();
    for name in [
        "campaign.json",
        "environment.json",
        "cells.csv",
        "curves.csv",
        "bd-rate.csv",
        "qualification.json",
        "summary.md",
        "failures.md",
        "confirmation.md",
    ] {
        let source = root.join(name);
        if !source.exists() {
            continue;
        }
        let target = destination.join(name);
        std::fs::copy(&source, &target)?;
        let hash = file_sha256(&source)?;
        ensure!(
            file_sha256(&target)? == hash,
            "archive SHA-256 mismatch: {name}"
        );
        hashes.push(json!({"path": name, "sha256": hash}));
    }
    archive_cells(&root.join("cells"), &destination.join("cells"), &mut hashes)?;
    if root.join("compatibility").exists() {
        archive_cells(
            &root.join("compatibility"),
            &destination.join("compatibility"),
            &mut hashes,
        )?;
    }
    let sanity = root.join("metric-sanity");
    archive_cells(&sanity, &destination.join("metric-sanity"), &mut hashes)?;
    write_json(
        &root.join("archive-verification.json"),
        &json!({"destination": destination, "files": hashes, "verified": true}),
    )?;
    Ok(())
}

fn archive_cells(source: &Path, destination: &Path, hashes: &mut Vec<Value>) -> anyhow::Result<()> {
    std::fs::create_dir_all(destination)?;
    for entry in std::fs::read_dir(source)? {
        let entry = entry?;
        let path = entry.path();
        let target = destination.join(entry.file_name());
        if path.is_dir() {
            archive_cells(&path, &target, hashes)?;
        } else {
            let name = entry.file_name().to_string_lossy().into_owned();
            let representative = source.ancestors().take(2).any(|p| {
                p.file_name()
                    .is_some_and(|name| name.to_string_lossy().contains("-cq-24-BASE"))
            }) || source.to_string_lossy().contains("compatibility");
            let failed = source
                .ancestors()
                .take(2)
                .any(|p| p.join("failure.log").exists());
            let retained =
                name.ends_with(".json") || name.ends_with(".log") || representative || failed;
            if !retained {
                continue;
            }
            std::fs::copy(&path, &target)?;
            let hash = file_sha256(&path)?;
            ensure!(
                file_sha256(&target)? == hash,
                "archive SHA-256 mismatch: {}",
                target.display()
            );
            hashes.push(json!({"path": target, "sha256": hash}));
        }
    }
    Ok(())
}

pub fn run(args: &CampaignArgs) -> anyhow::Result<ExitCode> {
    let manifest: Manifest = serde_json::from_slice(&std::fs::read(&args.manifest)?)?;
    ensure!(!manifest.clips.is_empty(), "no reference clips");
    ensure!(
        manifest
            .clips
            .iter()
            .map(|c| &c.name)
            .collect::<std::collections::BTreeSet<_>>()
            .len()
            == manifest.clips.len(),
        "duplicate clip names"
    );
    ensure!(
        manifest.model == "vmaf_v0.6.1 (FFmpeg embedded default)",
        "matrix scoring uses the embedded default model"
    );
    let root = super::abspath(&args.output)?;
    ensure!(
        !root.to_string_lossy().starts_with("\\\\"),
        "active campaign must use local disk"
    );
    std::fs::create_dir_all(&root)?;
    let _tree_lock = crate::host_lock::acquire(
        crate::host_lock::LockKind::Tree,
        Some(&root),
        "encoder quality campaign",
    )?;
    let _device_lock = crate::host_lock::acquire(
        crate::host_lock::LockKind::Device,
        None,
        "encoder quality campaign",
    )?;
    let _awake = host::Awake::new()?;
    let head = capture(Path::new("git"), &["rev-parse", "HEAD"])?;
    let build: Value = serde_json::from_slice(&std::fs::read(&manifest.build_receipt)?)?;
    ensure!(
        build["head"] == head.trim()
            && build["configuration"] == "Release"
            && build["probe_sha256"] == file_sha256(&manifest.probe)?,
        "frozen Release build receipt does not match source/probe"
    );
    ensure!(
        capture(Path::new("git"), &["status", "--porcelain"])?
            .trim()
            .is_empty(),
        "source tree must be clean"
    );
    let mut references = Vec::new();
    for clip in &manifest.clips {
        ensure!(
            !clip.qualified
                || (clip.source_sha256.len() == 64
                    && clip.source_sha256.bytes().all(|b| b.is_ascii_hexdigit())),
            "qualified reference needs an exact source hash"
        );
        if let Some(source) = &clip.source_path {
            ensure!(
                file_sha256(source)? == clip.source_sha256,
                "source-file hash differs for {}",
                clip.name
            );
        }
        ensure!(
            clip.name
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-'),
            "clip name must be a safe identifier"
        );
        ensure!(
            clip.path.is_absolute() && !clip.path.to_string_lossy().starts_with("\\\\"),
            "references must be absolute local paths"
        );
        let (line, _) = y4m::read_header_line(&clip.path)?;
        let header = y4m::parse_header_line(&line)?;
        ensure!(
            header.width == 1920
                && header.height == 1080
                && header.fps_num == 60
                && header.fps_den == 1,
            "reference must be 1080p60"
        );
        ensure!(
            line.split_whitespace()
                .any(|v| matches!(v, "C420jpeg" | "C420mpeg2" | "C420")),
            "reference must be 8-bit 4:2:0"
        );
        let duration = y4m::clip_duration_seconds(&clip.path)?;
        ensure!(
            duration * 60.0 >= 200.0,
            "reference needs at least 200 frames"
        );
        references.push(json!({"clip": clip, "sha256": file_sha256(&clip.path)?, "header": line, "frames": duration * 60.0, "duration": duration}));
    }
    let capabilities = capture(&manifest.probe, &["--capabilities"])?;
    let environment = json!({"schema": SCHEMA, "head": head.trim(), "dirty": false, "probe": manifest.probe,
        "probe_sha256": file_sha256(&manifest.probe)?, "configuration": "Release", "build_receipt": build, "capabilities": capabilities,
        "gpu_driver": capture(Path::new("nvidia-smi"), &["--query-gpu=name,driver_version,uuid", "--format=csv,noheader"] )?,
        "nvenc_sdk": manifest.nvenc_sdk, "ffmpeg": manifest.ffmpeg, "ffmpeg_sha256": file_sha256(&manifest.ffmpeg)?,
        "ffmpeg_version": capture(&manifest.ffmpeg, &["-version"] )?, "libvmaf": manifest.libvmaf, "model": manifest.model,
        "ffprobe": manifest.ffprobe, "ffprobe_sha256": file_sha256(&manifest.ffprobe)?, "references": references,
        "tuning": [variants("cq").into_iter().map(|(name, tuning)| json!({"name": name, "tuning": tuning})).collect::<Vec<_>>(), variants("vbr").into_iter().map(|(name, tuning)| json!({"name": name, "tuning": tuning})).collect::<Vec<_>>()],
        "runner_sha256": file_sha256(&std::env::current_exe()?)?});
    for runtime in build["runtime"]
        .as_array()
        .context("build receipt must identify runtime DLLs")?
    {
        let path = runtime["path"].as_str().context("runtime DLL path")?;
        ensure!(
            file_sha256(Path::new(path))? == runtime["sha256"],
            "runtime DLL hash differs"
        );
    }
    let env_path = root.join("environment.json");
    if env_path.exists() {
        let saved: Value = serde_json::from_slice(&std::fs::read(&env_path)?)?;
        ensure!(
            saved == environment,
            "campaign identity changed; preserve this root and select a new one"
        );
    } else {
        write_json(&env_path, &environment)?;
    }
    let campaign_path = root.join("campaign.json");
    let started = if campaign_path.exists() {
        serde_json::from_slice::<Value>(&std::fs::read(&campaign_path)?)?["started"]
            .as_u64()
            .context("campaign start missing")?
    } else {
        now()
    };
    let available = host::free(&root)?;
    let confirmation_points_per_rc = 3 * 2 * 4;
    let cq_points = variants("cq").len() * 3 * 4 + confirmation_points_per_rc;
    let vbr_points = variants("vbr").len() * 3 * 4 + confirmation_points_per_rc;
    let vbr_peak_bps = points("vbr").iter().sum::<i64>() as f64 / 4.0 * 1500.0;
    let estimated = references
        .iter()
        .map(|r| r["duration"].as_f64().unwrap_or(0.0))
        .sum::<f64>()
        * (cq_points as f64 * 100_000_000.0 + vbr_points as f64 * vbr_peak_bps)
        / 8.0;
    ensure!(
        available as f64 > (manifest.reserve_gib * 1024 * 1024 * 1024) as f64 + estimated,
        "insufficient campaign space above reserve"
    );
    let mut campaign = json!({"schema": SCHEMA, "environment": environment, "manifest": manifest, "started": started, "expected_main": manifest.clips.len() * 180,
        "available_before": available, "estimated_media_bytes": estimated, "storage_estimate": {"cq_assumed_bps": 100_000_000, "vbr_average_peak_bps": vbr_peak_bps, "cq_points_per_clip": cq_points, "vbr_points_per_clip": vbr_points, "includes_confirmation": true}, "reserve_gib": manifest.reserve_gib, "run_order": "clip, codec, RC, point, variant; baseline first at each point"});
    write_json(&campaign_path, &campaign)?;
    let sanity_dir = root.join("metric-sanity");
    if !root.join("metric-sanity-passed.json").exists() {
        std::fs::create_dir_all(&sanity_dir)?;
        ensure!(
            sanity::run_metric_sanity(
                &manifest.ffmpeg.to_string_lossy(),
                &manifest.clips[0].path,
                &sanity_dir
            )?,
            "metric sanity failed; campaign aborted"
        );
        write_json(&root.join("metric-sanity-passed.json"), &environment)?;
    } else {
        ensure!(
            serde_json::from_slice::<Value>(&std::fs::read(
                root.join("metric-sanity-passed.json")
            )?)? == environment,
            "sanity identity differs"
        );
    }
    let sanity_identity: Value = serde_json::from_slice(&std::fs::read(
        sanity_dir.join("sanity-identity.vmaf.json"),
    )?)?;
    ensure!(
        sanity_identity["version"] == manifest.libvmaf,
        "common scoring identity differs from manifest; campaign aborted"
    );
    let mut cells = Vec::new();
    for clip in &manifest.clips {
        for codec in ["h264", "hevc", "av1"] {
            for rc in ["cq", "vbr"] {
                ensure!(
                    capture(Path::new("git"), &["rev-parse", "HEAD"])? == head
                        && capture(Path::new("git"), &["status", "--porcelain"])?
                            .trim()
                            .is_empty(),
                    "source changed during measurement"
                );
                ensure!(
                    file_sha256(&manifest.probe)? == environment["probe_sha256"],
                    "frozen probe changed"
                );
                ensure!(
                    file_sha256(&manifest.ffmpeg)? == environment["ffmpeg_sha256"],
                    "scoring executable changed"
                );
                ensure!(
                    capture(
                        Path::new("nvidia-smi"),
                        &[
                            "--query-gpu=name,driver_version,uuid",
                            "--format=csv,noheader"
                        ]
                    )? == environment["gpu_driver"],
                    "GPU/driver identity changed"
                );
                for point in points(rc) {
                    for (_, tuning) in variants(rc) {
                        cells.push(cell_run(
                            &manifest,
                            &environment,
                            clip,
                            ("main", codec, rc, point, &tuning),
                            &root,
                        )?);
                        reports(&root, &manifest, &cells, &[], &[])?;
                        campaign["completed"] =
                            json!(cells.iter().filter(|c| c["status"] == "COMPLETED").count());
                        campaign["failed"] =
                            json!(cells.iter().filter(|c| c["status"] == "FAILED").count());
                        campaign["na"] =
                            json!(cells.iter().filter(|c| c["status"] == "N/A").count());
                        campaign["resumed"] =
                            json!(cells.iter().filter(|c| c["resumed"] == true).count());
                        campaign["last_cell"] = cells
                            .last()
                            .map(|c| c["key"].clone())
                            .unwrap_or(Value::Null);
                        write_json(&campaign_path, &campaign)?;
                    }
                }
            }
        }
    }
    for (index, clip) in manifest.clips.iter().enumerate() {
        ensure!(
            file_sha256(&clip.path)? == environment["references"][index]["sha256"],
            "reference changed; aggregation aborted"
        );
    }
    let initial = comparisons(&manifest, &cells, "main");
    let mut selected = Vec::new();
    for codec in ["h264", "hevc", "av1"] {
        for rc in ["cq", "vbr"] {
            let mut candidates: Vec<_> = initial
                .iter()
                .filter(|r| {
                    r["codec"] == codec
                        && r["rc"] == rc
                        && r["references_qualified"] == true
                        && r["service_pass"] == true
                        && r["median"].as_f64().is_some_and(|v| v <= -4.5)
                        && r["worst"].as_f64().is_some_and(|v| v <= 2.5)
                        && r["errors"].as_array().is_some_and(Vec::is_empty)
                })
                .collect();
            candidates.sort_by(|a, b| {
                b["quality_pass"]
                    .as_bool()
                    .cmp(&a["quality_pass"].as_bool())
                    .then_with(|| {
                        a["median"]
                            .as_f64()
                            .unwrap()
                            .total_cmp(&b["median"].as_f64().unwrap())
                    })
            });
            for candidate in candidates.into_iter().take(1) {
                let name = candidate["variant"].as_str().context("variant")?;
                let tuning = variants(rc)
                    .into_iter()
                    .find(|(v, _)| *v == name)
                    .context("variant definition")?
                    .1;
                let base = variants(rc).remove(0).1;
                for clip in &manifest.clips {
                    for point in points(rc) {
                        for t in [&base, &tuning] {
                            let value = cell_run(
                                &manifest,
                                &environment,
                                clip,
                                ("confirmation", codec, rc, point, t),
                                &root,
                            )?;
                            if !cells.iter().any(|c| c["key"] == value["key"]) {
                                cells.push(value);
                            }
                        }
                    }
                }
                selected.push(json!({"codec": codec, "rc": rc, "candidate": name}));
            }
        }
    }
    let mut confirmation = if selected.is_empty() {
        Vec::new()
    } else {
        comparisons(&manifest, &cells, "confirmation")
    };
    confirmation.retain(|r| {
        selected.iter().any(|s| {
            r["codec"] == s["codec"] && r["rc"] == s["rc"] && r["variant"] == s["candidate"]
        })
    });
    for result in &mut confirmation {
        let initial_pass = initial.iter().any(|r| {
            r["codec"] == result["codec"]
                && r["rc"] == result["rc"]
                && r["variant"] == result["variant"]
                && r["quality_pass"] == true
                && r["service_pass"] == true
        });
        if initial_pass && result["quality_pass"] == true && result["service_pass"] == true {
            match compatibility(
                &manifest,
                result["codec"].as_str().unwrap(),
                result["rc"].as_str().unwrap(),
                result["variant"].as_str().unwrap(),
                &root,
            ) {
                Ok(evidence) => {
                    result["compatibility"] = evidence;
                    result["compatibility_complete"] = json!(true);
                    result["verdict"] = json!("CONFIRMED");
                }
                Err(error) => {
                    result["compatibility_error"] = json!(format!("{error:#}"));
                    result["verdict"] = json!("INCONCLUSIVE");
                }
            }
        } else {
            result["verdict"] = json!("INCONCLUSIVE");
        }
    }
    reports(&root, &manifest, &cells, &initial, &confirmation)?;
    if !selected.is_empty() {
        std::fs::write(
            root.join("confirmation.md"),
            format!(
                "# Winner-only confirmation\n\nSelected: {selected:?}\n\nSee qualification.json. Quality/service confirmation alone does not establish editor/container compatibility.\n"
            ),
        )?;
    }
    for (index, clip) in manifest.clips.iter().enumerate() {
        ensure!(
            file_sha256(&clip.path)? == environment["references"][index]["sha256"],
            "reference changed during campaign"
        );
    }
    ensure!(
        capture(Path::new("git"), &["rev-parse", "HEAD"])? == head
            && capture(Path::new("git"), &["status", "--porcelain"])?
                .trim()
                .is_empty(),
        "source changed during campaign"
    );
    campaign["finished"] = json!(now());
    campaign["counts"] = json!({"completed": cells.iter().filter(|c| c["status"] == "COMPLETED").count(), "failed": cells.iter().filter(|c| c["status"] == "FAILED").count(), "na": cells.iter().filter(|c| c["status"] == "N/A").count(), "resumed": cells.iter().filter(|c| c["resumed"] == true).count(), "total": cells.len()});
    campaign["available_after"] = json!(host::free(&root)?);
    campaign["selected_confirmation"] = json!(selected);
    write_json(&campaign_path, &campaign)?;
    if let Some(destination) = &manifest.archive {
        archive(&root, destination)?;
    }
    Ok(ExitCode::SUCCESS)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn exact_four_point_campaign_is_not_the_historical_matrix() {
        assert_eq!(points("cq"), [19, 24, 30, 36]);
        assert_eq!(points("vbr"), [3000, 6000, 12000, 24000]);
    }

    #[test]
    fn thresholds_and_complete_reference_set_are_load_bearing() {
        assert!(quality_gate(&[-8.0, -5.0, 2.0]));
        assert!(!quality_gate(&[-8.0, -4.8, 0.0]));
        assert!(!quality_gate(&[-8.0, -6.0, 2.4]));
        assert!(!quality_gate(&[-9.0, -8.0]));
        assert!(!quality_gate(&[f64::NAN, -9.0, -8.0]));
    }

    #[test]
    fn resume_requires_exact_identity_and_success() {
        let identity =
            json!({"schema": 1, "head": "a", "probe": "b", "reference": "c", "scorer": "d"});
        let saved = json!({"identity": identity, "status": "COMPLETED"});
        assert!(reusable(&saved, &identity));
        for field in ["schema", "head", "probe", "reference", "scorer"] {
            let mut changed = identity.clone();
            changed[field] = json!("different");
            assert!(!reusable(&saved, &changed));
        }
        let mut failed = saved;
        failed["status"] = json!("FAILED");
        assert!(!reusable(&failed, &identity));
    }

    #[test]
    fn failed_point_cannot_form_a_load_bearing_curve() {
        let mut cells: Vec<_> = points("cq").iter().map(|point| json!({"status": "COMPLETED", "identity": {"phase": "main", "clip": "gameplay", "codec": "h264", "rc": "cq", "variant": "BASE", "value": point}})).collect();
        assert!(curve(&cells, "main", "gameplay", "h264", "cq", "BASE").is_some());
        cells[2]["status"] = json!("FAILED");
        assert!(curve(&cells, "main", "gameplay", "h264", "cq", "BASE").is_none());
        cells[2]["status"] = json!("COMPLETED");
        cells[2]["identity"]["codec"] = json!("hevc");
        assert!(curve(&cells, "main", "gameplay", "h264", "cq", "BASE").is_none());
    }

    #[test]
    fn authoritative_capabilities_never_force_unsupported_tuning() {
        let caps =
            "[probe] CAPS codec=h264 supported=1 max_bframes=2 b_ref=2 lookahead=0 temporal_aq=1";
        let candidate = variants("cq")
            .into_iter()
            .find(|(name, _)| *name == "BL")
            .unwrap()
            .1;
        assert_eq!(
            unsupported(caps, "h264", &candidate).unwrap().as_deref(),
            Some("Lookahead unsupported")
        );
        let base = variants("cq").remove(0).1;
        assert!(unsupported(caps, "h264", &base).unwrap().is_none());
        assert!(unsupported(&format!("{caps}\n{caps}"), "h264", &base).is_err());
    }

    #[test]
    fn resolved_features_must_match_the_measured_identity() {
        let tuning = variants("cq")
            .into_iter()
            .find(|(name, _)| *name == "BLT")
            .unwrap()
            .1;
        let log = "[probe] RESOLVED bframes=2 b_ref=middle lookahead=16 spatial_aq=0 temporal_aq=1 multipass=single slots=23 output_depth=23";
        assert!(resolved_matches(log, &tuning));
        assert!(!resolved_matches(
            &log.replace("lookahead=16", "lookahead=0"),
            &tuning
        ));
        assert!(!resolved_matches(
            &log.replace("b_ref=middle", "b_ref=off"),
            &tuning
        ));
    }
}
