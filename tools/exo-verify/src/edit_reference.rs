//! Opt-in, file-backed editor export checks against externally frozen references.

use crate::context::{App, Context};
use crate::scenario::{Lane, Step, Stop};
use anyhow::{Context as _, Result, bail, ensure};
use serde::Deserialize;
use serde_json::{Value, json};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::time::{Duration, Instant};

#[derive(clap::Args)]
pub struct Args {
    #[arg(long)]
    pub manifest: PathBuf,
    #[arg(long, required_unless_present = "validate_only")]
    pub product: Option<PathBuf>,
    #[arg(long, required_unless_present = "validate_only")]
    pub out: Option<PathBuf>,
    /// Validate all declared inputs and frozen references without launching the product.
    #[arg(long)]
    pub validate_only: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    schema: String,
    max_output_bytes: u64,
    cases: Vec<Case>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Case {
    id: String,
    source: PathBuf,
    source_sha256: String,
    oracle: PathBuf,
    oracle_sha256: String,
    expect: Expected,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
enum Expected {
    Audio {
        source_rate: u32,
        source_channels: usize,
        source_frames: usize,
        output_rate: u32,
        output_channels: usize,
        channel_map: Vec<usize>,
        channel_gain: Vec<f64>,
        detector: Detector,
    },
    Video {
        output_fps_num: u32,
        output_fps_den: u32,
        pts_tolerance_us: u64,
        barcode_threshold: u8,
        allowed_paths: Vec<String>,
    },
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Detector {
    window_samples: usize,
    peak_offset_samples: usize,
    sum_relative_tolerance: f64,
    peak_min_ratio: f64,
    peak_max_ratio: f64,
    quiet_pcm24: i32,
}

#[derive(Debug, Clone)]
struct Event {
    channel: usize,
    sample: usize,
    pcm16: i32,
}

#[derive(Debug, Clone)]
struct Frame {
    id: u32,
    pts_us: i64,
    end_us: i64,
}

fn parse_manifest(text: &str) -> Result<Manifest> {
    let manifest: Manifest = serde_json::from_str(text)?;
    ensure!(
        manifest.schema == "exosnap.edit-reference/1",
        "unknown manifest schema"
    );
    ensure!(
        (1..=33_554_432).contains(&manifest.max_output_bytes),
        "output budget must be1..33554432 bytes"
    );
    ensure!(
        (1..=9).contains(&manifest.cases.len()),
        "expected1..9 compact cases"
    );
    let mut ids = std::collections::BTreeSet::new();
    for case in &manifest.cases {
        ensure!(
            !case.id.is_empty()
                && case.id.len() <= 64
                && case
                    .id
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_'),
            "unsafe case id"
        );
        ensure!(ids.insert(&case.id), "duplicate case id");
        ensure!(
            crate::bundle::is_sha256(&case.source_sha256)
                && crate::bundle::is_sha256(&case.oracle_sha256),
            "invalid SHA256"
        );
        match &case.expect {
            Expected::Audio {
                source_rate,
                source_channels,
                source_frames,
                output_rate,
                output_channels,
                channel_map,
                channel_gain,
                detector,
            } => {
                ensure!(
                    [44100, 48000, 96000].contains(source_rate)
                        && (1..=2).contains(source_channels),
                    "unsupported audio source"
                );
                ensure!(
                    *source_frames == *source_rate as usize * 4
                        && *output_rate == 48000
                        && *output_channels == 2,
                    "expected four-second48k stereo output"
                );
                ensure!(
                    channel_map.len() == *output_channels
                        && channel_map.iter().all(|c| *c < *source_channels),
                    "invalid channel map"
                );
                ensure!(
                    channel_gain.len() == *output_channels
                        && channel_gain
                            .iter()
                            .all(|g| g.is_finite() && *g > 0.0 && *g <= 4.0),
                    "invalid channel gain"
                );
                ensure!(
                    (1..=256).contains(&detector.window_samples)
                        && detector.peak_offset_samples <= detector.window_samples,
                    "invalid detector window"
                );
                ensure!(
                    detector.sum_relative_tolerance.is_finite()
                        && (0.0..=0.25).contains(&detector.sum_relative_tolerance),
                    "invalid area tolerance"
                );
                ensure!(
                    detector.peak_min_ratio.is_finite()
                        && detector.peak_max_ratio.is_finite()
                        && detector.peak_min_ratio > 0.0
                        && detector.peak_min_ratio <= detector.peak_max_ratio
                        && detector.peak_max_ratio <= 2.0
                        && (0..=4096).contains(&detector.quiet_pcm24),
                    "invalid peak/quiet tolerance"
                );
            }
            Expected::Video {
                output_fps_num,
                output_fps_den,
                pts_tolerance_us,
                barcode_threshold,
                allowed_paths,
            } => {
                ensure!(
                    *output_fps_num > 0
                        && *output_fps_den > 0
                        && u64::from(*output_fps_num) <= 240 * u64::from(*output_fps_den),
                    "invalid output fps"
                );
                ensure!(
                    *pts_tolerance_us <= 2000 && (32..=224).contains(barcode_threshold),
                    "invalid video tolerance"
                );
                ensure!(
                    !allowed_paths.is_empty()
                        && allowed_paths.len() <= 2
                        && allowed_paths.iter().all(|p| p == "Copy" || p == "Render"),
                    "invalid review paths"
                );
            }
        }
    }
    Ok(manifest)
}

fn judge_audio(bytes: &[u8], events: &[Event], expected: &Expected) -> Result<()> {
    let Expected::Audio {
        source_rate,
        source_channels,
        source_frames,
        output_rate,
        output_channels,
        channel_map,
        channel_gain,
        detector,
    } = expected
    else {
        bail!("not audio")
    };
    let frames = *source_frames * *output_rate as usize / *source_rate as usize;
    ensure!(
        bytes.len() == frames * output_channels * 3,
        "PCM frame count/channel width mismatch: {} bytes",
        bytes.len()
    );
    ensure!(
        !events.is_empty()
            && events.iter().all(|e| e.channel < *source_channels
                && e.sample < *source_frames
                && e.pcm16 != 0
                && (-32768..=32767).contains(&e.pcm16)),
        "invalid impulse table"
    );
    let pcm: Vec<i32> = bytes
        .chunks_exact(3)
        .map(|b| i32::from_le_bytes([b[0], b[1], b[2], if b[2] & 128 != 0 { 255 } else { 0 }]))
        .collect();
    if source_rate == output_rate {
        let mut reference = vec![0i32; pcm.len()];
        for event in events {
            for (channel, source) in channel_map.iter().enumerate() {
                if *source == event.channel {
                    reference[event.sample * output_channels + channel] =
                        (f64::from(event.pcm16) / 32768.0 * 8_388_607.0 * channel_gain[channel])
                            .round() as i32;
                }
            }
        }
        if let Some((index, (actual, wanted))) = pcm
            .iter()
            .zip(&reference)
            .enumerate()
            .find(|(_, (a, b))| a != b)
        {
            bail!(
                "exact PCM mismatch at sample{} channel{}: {actual} versus {wanted}",
                index / output_channels,
                index % output_channels
            );
        }
    } else {
        let ratio = f64::from(*output_rate) / f64::from(*source_rate);
        let mut relevant = vec![false; pcm.len()];
        for event in events {
            let center = (event.sample as f64 * ratio).round() as usize;
            let from = center.saturating_sub(detector.window_samples).min(frames);
            let to = center
                .saturating_add(detector.window_samples + 1)
                .min(frames);
            for (channel, source) in channel_map.iter().enumerate() {
                if *source != event.channel {
                    continue;
                }
                for frame in from..to {
                    relevant[frame * output_channels + channel] = true;
                }
                // Resampler edge truncation is excluded from the interior area/peak oracle.
                if center < detector.window_samples || center + detector.window_samples >= frames {
                    continue;
                }
                let (peak_at, peak) = (from..to)
                    .map(|f| (f, pcm[f * output_channels + channel]))
                    .max_by_key(|(_, v)| v.abs())
                    .context("empty impulse window")?;
                let sum: i64 = (from..to)
                    .map(|f| i64::from(pcm[f * output_channels + channel]))
                    .sum();
                let amplitude =
                    f64::from(event.pcm16) / 32768.0 * 8_388_607.0 * channel_gain[channel];
                let wanted_sum = amplitude * ratio;
                let wanted_peak = amplitude.abs() * ratio.min(1.0);
                ensure!(
                    peak_at.abs_diff(center) <= detector.peak_offset_samples,
                    "resampled impulse timing mismatch"
                );
                ensure!(
                    (sum as f64 - wanted_sum).abs()
                        <= wanted_sum.abs() * detector.sum_relative_tolerance,
                    "resampled signed area mismatch"
                );
                ensure!(
                    f64::from(peak) * amplitude > 0.0
                        && f64::from(peak.abs()) >= wanted_peak * detector.peak_min_ratio
                        && f64::from(peak.abs()) <= wanted_peak * detector.peak_max_ratio,
                    "resampled peak/sign mismatch"
                );
            }
        }
        ensure!(
            pcm.iter()
                .zip(&relevant)
                .all(|(v, active)| *active || v.abs() <= detector.quiet_pcm24),
            "unrelated signal/channel outside impulse windows"
        );
    }
    Ok(())
}

fn judge_video(
    bars: &[u8],
    pts: &[i64],
    source: &[Frame],
    expected: &Expected,
    path: &str,
) -> Result<()> {
    let Expected::Video {
        output_fps_num,
        output_fps_den,
        pts_tolerance_us,
        barcode_threshold,
        allowed_paths,
    } = expected
    else {
        bail!("not video")
    };
    ensure!(
        allowed_paths.iter().any(|p| p == path),
        "unexpected export review path {path}"
    );
    ensure!(
        !source.is_empty() && !pts.is_empty() && bars.len() == pts.len() * 10,
        "barcode/frame count mismatch"
    );
    let end = source.last().unwrap().end_us;
    let expected_count = if path == "Copy" {
        source.len()
    } else {
        ((end as u128 * u128::from(*output_fps_num))
            .div_ceil(1_000_000 * u128::from(*output_fps_den))) as usize
    };
    ensure!(
        pts.len() == expected_count,
        "output frame count{} versus{expected_count}",
        pts.len()
    );
    for (index, actual) in pts.iter().enumerate() {
        let time = if path == "Copy" {
            source[index].pts_us
        } else {
            (index as u128 * 1_000_000 * u128::from(*output_fps_den) / u128::from(*output_fps_num))
                as i64
        };
        ensure!(
            actual.abs_diff(time) <= *pts_tolerance_us && (index == 0 || *actual > pts[index - 1]),
            "frame{index} PTS{actual} versus{time}"
        );
        let wanted = source
            .iter()
            .find(|f| time >= f.pts_us && time < f.end_us)
            .context("output time outside independent source intervals")?;
        let label = bars[index * 10..index * 10 + 10]
            .iter()
            .enumerate()
            .fold(0u32, |id, (bit, v)| {
                id | (u32::from(*v >= *barcode_threshold) << bit)
            });
        ensure!(
            label == wanted.id,
            "frame{index} barcode{label} versus{}",
            wanted.id
        );
    }
    Ok(())
}

fn csv_rows(text: &str, header: &str) -> Result<Vec<Vec<String>>> {
    let mut lines = text.lines();
    let normalize = |line: &str| -> Result<Vec<String>> {
        line.split(',')
            .map(|part| {
                let value = part.trim().trim_matches('"');
                ensure!(!value.contains('"'), "escaped CSV fields are unsupported");
                Ok(value.to_string())
            })
            .collect()
    };
    ensure!(
        normalize(lines.next().context("missing CSV header")?)?.join(",") == header,
        "unknown CSV header"
    );
    let rows: Vec<_> = lines
        .filter(|s| !s.trim().is_empty())
        .map(normalize)
        .collect::<Result<_>>()?;
    ensure!(
        !rows.is_empty() && rows.len() <= 1000,
        "expected1..1000 oracle rows"
    );
    Ok(rows)
}

fn events(text: &str) -> Result<Vec<Event>> {
    let result: Vec<Event> = csv_rows(text, "channel,sample_index,pcm_s16")?
        .into_iter()
        .map(|row| {
            ensure!(row.len() == 3, "invalid event columns");
            Ok(Event {
                channel: row[0].parse()?,
                sample: row[1].parse()?,
                pcm16: row[2].parse()?,
            })
        })
        .collect::<Result<_>>()?;
    let mut locations = std::collections::BTreeSet::new();
    ensure!(
        result.iter().all(|e| e.channel < 2
            && e.sample < 384000
            && e.pcm16 != 0
            && (-32768..=32767).contains(&e.pcm16)
            && locations.insert((e.channel, e.sample))),
        "invalid or duplicate impulse event"
    );
    Ok(result)
}

fn frames(text: &str) -> Result<Vec<Frame>> {
    let mut result = Vec::new();
    for row in csv_rows(
        text,
        "frame_id,pts_ticks,duration_ticks,time_base_num,time_base_den,barcode_lsb_first",
    )? {
        ensure!(row.len() == 6, "invalid frame columns");
        let id: u32 = row[0].parse()?;
        let pts: i64 = row[1].parse()?;
        let duration: i64 = row[2].parse()?;
        let num: i64 = row[3].parse()?;
        let den: i64 = row[4].parse()?;
        ensure!(
            id < 1024 && pts >= 0 && duration > 0 && num > 0 && den > 0,
            "invalid source interval"
        );
        ensure!(
            row[5].len() == 10
                && row[5]
                    .bytes()
                    .enumerate()
                    .all(|(bit, b)| b == if id & (1 << bit) == 0 { b'0' } else { b'1' }),
            "frame label disagrees with barcode oracle"
        );
        let to_us = |ticks: i128| -> Result<i128> {
            Ok(ticks
                .checked_mul(i128::from(num))
                .and_then(|v| v.checked_mul(1_000_000))
                .context("source timestamp overflow")?
                / i128::from(den))
        };
        let start = to_us(i128::from(pts))?;
        let end = to_us(i128::from(pts) + i128::from(duration))?;
        ensure!(
            start >= 0 && end > start && end <= 12_000_000,
            "source interval outside compact budget"
        );
        if let Some(previous) = result.last() {
            let previous: &Frame = previous;
            ensure!(
                previous.end_us == start as i64 && id > previous.id,
                "source intervals must be contiguous and ordered"
            );
        } else {
            ensure!(start == 0, "source oracle must start at zero");
        }
        result.push(Frame {
            id,
            pts_us: start as i64,
            end_us: end as i64,
        });
    }
    Ok(result)
}

enum Oracle {
    Audio(Vec<Event>),
    Video(Vec<Frame>),
}

fn review_reserve(
    review: &Value,
    source_bytes: u64,
    oracle: &Oracle,
    expected: &Expected,
) -> Result<u64> {
    let duration_us = match (oracle, expected) {
        (
            Oracle::Audio(_),
            Expected::Audio {
                source_frames,
                source_rate,
                ..
            },
        ) => (*source_frames as u64) * 1_000_000 / u64::from(*source_rate),
        (Oracle::Video(frames), Expected::Video { .. }) => {
            ensure!(
                review["path"] == "Copy",
                "video Render plan has no exposed bitrate/geometry bound"
            );
            frames.last().context("empty video reference")?.end_us as u64
        }
        _ => bail!("oracle/expectation kind mismatch"),
    };
    ensure!(
        (1..=12_000_000).contains(&duration_us),
        "invalid bounded source duration"
    );
    // Copy describes the video path and can still include generated PCM tracks.
    let tracks = review["outputAudioTracks"]
        .as_array()
        .context("declared output audio tracks missing")?;
    ensure!(tracks.len() <= 8, "too many declared output audio tracks");
    let frames = (duration_us * 48000).div_ceil(1_000_000);
    let mut pcm_bytes = 0u64;
    for track in tracks {
        let channels = track["channels"]
            .as_u64()
            .context("declared channel count missing")?;
        ensure!(
            track["codec"] == "PCM24"
                && track["sampleRate"] == 48000
                && (1..=2).contains(&channels),
            "output audio codec/rate/channels have no supported byte bound"
        );
        pcm_bytes = pcm_bytes
            .checked_add(frames * channels * 3)
            .context("output byte bound overflow")?;
    }
    let (video_bytes, decoded_bytes) = match expected {
        Expected::Audio {
            output_channels, ..
        } => {
            ensure!(
                tracks.len() == 1 && tracks[0]["channels"] == *output_channels as u64,
                "audio review must declare the expected single stereo track"
            );
            (0, frames * (*output_channels as u64) * 3)
        }
        Expected::Video { .. } => (source_bytes, 1001 * 10),
    };
    Ok(video_bytes + pcm_bytes + decoded_bytes + 1_048_576)
}

fn held_bytes(root: &Path) -> Result<u64> {
    let mut total = 0u64;
    for entry in std::fs::read_dir(root)? {
        let path = entry?.path();
        let meta = std::fs::symlink_metadata(&path)?;
        ensure!(
            !meta.file_type().is_symlink(),
            "unexpected symlink in retained output"
        );
        total = total
            .checked_add(if meta.is_dir() {
                held_bytes(&path)?
            } else {
                meta.len()
            })
            .context("retained byte count overflow")?;
    }
    Ok(total)
}

fn accepted(app: &mut App, command: &str, params: Value) -> Step<Value> {
    app.client
        .request(command, params, Duration::from_secs(30))?
        .map_err(|r| Stop::fail(format!("{command} refused: {r}")))
}

fn editor(app: &mut App) -> Step<Value> {
    accepted(app, "editor.snapshot", json!({}))
}

fn import_ready(snapshot: &Value) -> Result<bool> {
    let pending = snapshot["importPending"]
        .as_bool()
        .context("editor.snapshot carries no importPending flag")?;
    let clips = snapshot["clips"]
        .as_array()
        .context("editor.snapshot carries no clips array")?;
    let error = snapshot
        .get("workspaceError")
        .context("editor.snapshot carries no workspaceError")?;
    ensure!(
        pending || error.is_null(),
        "source import was refused: {error}"
    );
    Ok(!pending && !clips.is_empty())
}

fn export_wait(app: &mut App, root: &Path, budget: u64, states: &[&str]) -> Step<Value> {
    let deadline = Instant::now() + Duration::from_secs(180);
    loop {
        let state = accepted(app, "ui.getState", json!({}))?;
        if held_bytes(root)? > budget {
            let _ = app.call("export.cancel", json!({}));
            return Err(Stop::fail(
                "retained output exceeded the frozen byte envelope",
            ));
        }
        if states.iter().any(|s| state["exportState"] == *s) {
            return Ok(state);
        }
        if Instant::now() >= deadline {
            return Err(Stop::fail("export state did not settle within180s"));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn tool(ctx: &mut Context, name: &str, command: &mut Command) -> Step<crate::tools::Output> {
    let output = crate::tools::run(command, Duration::from_secs(60))?;
    let path = ctx.scenario_dir.join(format!("{name}.json"));
    std::fs::write(
        path,
        serde_json::to_vec_pretty(
            &json!({"command":format!("{command:?}"),"exit":output.code(),"stdout":output.stdout,"stderr":output.stderr}),
        )?,
    )?;
    if !output.success() {
        return Err(Stop::fail(format!(
            "{name} could not decode the product output; inspect its diagnostic JSON"
        )));
    }
    Ok(output)
}

fn run_case(
    ctx: &mut Context,
    case: &Case,
    source: &Path,
    source_probe: &Value,
    oracle: &Oracle,
    budget: u64,
) -> Step<Value> {
    ctx.evidence.put("sourceProbe", source_probe.clone());
    let product_log = ctx.scenario_dir.join("product.log");
    let mut app = ctx.launch_offscreen_logged(&[], &product_log)?;
    accepted(&mut app, "ui.navigate", json!({"page":"edit"}))?;
    let import_accepted = accepted(
        &mut app,
        "edit.importMedia",
        json!({"path":source,"append":true}),
    )?;
    ctx.evidence.put("importAccepted", import_accepted.clone());
    std::fs::write(
        ctx.scenario_dir.join("import-command.json"),
        serde_json::to_vec_pretty(&import_accepted)?,
    )?;
    let deadline = Instant::now() + Duration::from_secs(60);
    let mut previous = None;
    loop {
        let state = editor(&mut app)?;
        ctx.evidence.put("lastImportSnapshot", state.clone());
        if previous.as_ref() != Some(&state) {
            std::fs::write(
                ctx.scenario_dir.join("import-state.json"),
                serde_json::to_vec_pretty(&state)?,
            )?;
            previous = Some(state.clone());
        }
        if import_ready(&state).map_err(|e| Stop::fail(format!("import: {e:#}")))? {
            break;
        }
        if Instant::now() >= deadline {
            return Err(Stop::fail(format!("{} import did not complete", case.id)));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    if !crate::bundle::sha256_file(source)?
        .0
        .eq_ignore_ascii_case(&case.source_sha256)
    {
        return Err(Stop::infra(
            "reference source changed before the imported lease was acquired",
        ));
    }
    ctx.evidence.put("imported", editor(&mut app)?);
    accepted(
        &mut app,
        "export.setOptions",
        json!({"span":"entire","container":"mkv","mode":"finishedStereo"}),
    )?;
    accepted(&mut app, "export.start", json!({}))?;
    let state = export_wait(&mut app, &ctx.run_dir, budget, &["review", "failed"])?;
    let review = editor(&mut app)?["export"].clone();
    ctx.evidence.put("exportReview", review.clone());
    if state["exportState"] != "review" {
        return Err(Stop::fail(format!("export review failed: {review}")));
    }
    let path = review["review"]["path"]
        .as_str()
        .context("export review path missing")?;
    match &case.expect {
        Expected::Audio { .. } => {}
        Expected::Video { allowed_paths, .. } => {
            if !allowed_paths.iter().any(|p| p == path) {
                return Err(Stop::fail(format!("unexpected review path {path}")));
            }
            if path != "Copy" {
                return Err(Stop::unavailable(
                    "video Render review does not expose a bounded bitrate/geometry plan; export was not confirmed",
                ));
            }
        }
    }
    let reserve = review_reserve(
        &review["review"],
        std::fs::metadata(source)?.len(),
        oracle,
        &case.expect,
    )
    .map_err(|e| Stop::unavailable(format!("export review is not bounded: {e:#}")))?;
    ctx.evidence.put("reservedIncrementBytes", reserve);
    if held_bytes(&ctx.run_dir)?.saturating_add(reserve) > budget {
        return Err(Stop::unavailable(
            "full-source export and bounded oracle would exceed the frozen output envelope; export was not confirmed",
        ));
    }
    accepted(&mut app, "export.confirm", json!({}))?;
    let finished = export_wait(&mut app, &ctx.run_dir, budget, &["completed", "failed"])?;
    let exported = editor(&mut app)?["export"].clone();
    ctx.evidence.put("exportResult", exported.clone());
    if finished["exportState"] != "completed" {
        return Err(Stop::fail(format!("export failed: {exported}")));
    }
    let output = PathBuf::from(
        exported["outputPath"]
            .as_str()
            .context("missing export path")?,
    )
    .canonicalize()?;
    if output.parent() != Some(app.output.canonicalize()?.as_path()) {
        return Err(Stop::fail("export escaped its isolated output directory"));
    }
    let probe = crate::media::probe(&output)
        .map_err(|e| Stop::fail(format!("exported media probe failed: {e:#}")))?;
    ctx.evidence.put("outputProbe", probe.clone());
    let decoded = ctx.scenario_dir.join("decoded.raw");
    match oracle {
        Oracle::Audio(events) => {
            let audio = crate::media::streams(&probe, "audio");
            if audio.len() != 1
                || audio[0]["sample_rate"] != "48000"
                || audio[0]["channels"] != 2
                || audio[0]["codec_name"] != "pcm_s24le"
            {
                return Err(Stop::fail(
                    "export must contain one48kHz stereo PCM24 stream",
                ));
            }
            let ffmpeg = crate::tools::require("ffmpeg")?;
            tool(
                ctx,
                "decode-pcm",
                Command::new(ffmpeg)
                    .args([
                        "-nostdin",
                        "-hide_banner",
                        "-v",
                        "error",
                        "-n",
                        "-threads",
                        "1",
                        "-i",
                    ])
                    .arg(&output)
                    .args([
                        "-map",
                        "0:a:0",
                        "-t",
                        "4.001",
                        "-c:a",
                        "pcm_s24le",
                        "-f",
                        "s24le",
                    ])
                    .arg(&decoded),
            )?;
            judge_audio(&std::fs::read(&decoded)?, events, &case.expect)
                .map_err(|e| Stop::fail(format!("audio oracle: {e:#}")))?;
        }
        Oracle::Video(frames) => {
            let video = crate::media::streams(&probe, "video");
            if video.len() != 1 || video[0]["width"] != 320 || video[0]["height"] != 180 {
                return Err(Stop::fail(
                    "copied barcode output changed stream layout or geometry",
                ));
            }
            let actual_audio = crate::media::streams(&probe, "audio");
            let declared_audio = review["review"]["outputAudioTracks"]
                .as_array()
                .context("declared audio tracks missing")?;
            if actual_audio.len() != declared_audio.len()
                || actual_audio
                    .iter()
                    .zip(declared_audio)
                    .any(|(actual, declared)| {
                        actual["codec_name"] != "pcm_s24le"
                            || actual["sample_rate"] != "48000"
                            || actual["channels"] != declared["channels"]
                    })
            {
                return Err(Stop::fail(
                    "copied video output audio shape disagrees with its bounded review",
                ));
            }
            let ffmpeg = crate::tools::require("ffmpeg")?;
            tool(
                ctx,
                "decode-barcode",
                Command::new(ffmpeg)
                    .args([
                        "-nostdin",
                        "-hide_banner",
                        "-v",
                        "error",
                        "-n",
                        "-threads",
                        "1",
                        "-filter_threads",
                        "1",
                        "-i",
                    ])
                    .arg(&output)
                    .args([
                        "-map",
                        "0:v:0",
                        "-an",
                        "-frames:v",
                        "1001",
                        "-vf",
                        "format=gray,crop=320:1:0:20,scale=10:1:flags=neighbor",
                        "-pix_fmt",
                        "gray",
                        "-c:v",
                        "rawvideo",
                        "-fps_mode",
                        "passthrough",
                        "-f",
                        "rawvideo",
                    ])
                    .arg(&decoded),
            )?;
            let ffprobe = crate::tools::require("ffprobe")?;
            let output_frames = tool(
                ctx,
                "frame-pts",
                Command::new(ffprobe)
                    .args([
                        "-v",
                        "error",
                        "-select_streams",
                        "v:0",
                        "-read_intervals",
                        "%+#1001",
                        "-show_frames",
                        "-show_entries",
                        "frame=best_effort_timestamp_time",
                        "-of",
                        "json",
                    ])
                    .arg(&output),
            )?;
            let json: Value = serde_json::from_str(&output_frames.stdout)?;
            let pts = json["frames"]
                .as_array()
                .context("missing decoded frames")?
                .iter()
                .map(|f| {
                    let seconds: f64 = f["best_effort_timestamp_time"]
                        .as_str()
                        .context("missing frame PTS")?
                        .parse()?;
                    ensure!(
                        seconds.is_finite() && (0.0..=12.0).contains(&seconds),
                        "invalid frame PTS"
                    );
                    Ok((seconds * 1_000_000.0).round() as i64)
                })
                .collect::<Result<Vec<_>>>()?;
            judge_video(&std::fs::read(&decoded)?, &pts, frames, &case.expect, path)
                .map_err(|e| Stop::fail(format!("video oracle: {e:#}")))?;
            let Expected::Video {
                pts_tolerance_us, ..
            } = &case.expect
            else {
                unreachable!()
            };
            let duration = crate::media::f64_field(&probe["format"], "duration")
                .context("output duration missing")?;
            if !duration.is_finite()
                || (duration * 1_000_000.0 - frames.last().unwrap().end_us as f64).abs()
                    > *pts_tolerance_us as f64
            {
                return Err(Stop::fail(
                    "whole-source output endpoint disagrees with the frozen duration table",
                ));
            }
        }
    }
    if held_bytes(&ctx.run_dir)? > budget {
        return Err(Stop::fail("oracle artifacts exceeded the output envelope"));
    }
    let (sha, bytes) = crate::bundle::sha256_file(&output)?;
    let facts = json!({"output":output,"sha256":sha,"bytes":bytes,"reviewPath":path,"coverage":if matches!(oracle,Oracle::Video(_)) {"copy/export only; no evaluator or preview claim"} else {"file-backed exported PCM; no endpoint/preview claim"}});
    ctx.evidence.put("output", facts.clone());
    accepted(&mut app, "export.reset", json!({}))?;
    accepted(&mut app, "edit.close", json!({}))?;
    app.close_gracefully(Duration::from_secs(30))?;
    Ok(facts)
}

pub fn run(args: Args) -> Result<ExitCode> {
    let manifest_path = args.manifest.canonicalize()?;
    let protocol = std::fs::read_to_string(&manifest_path)?;
    let manifest = parse_manifest(&protocol)?;
    let base = manifest_path.parent().context("manifest parent missing")?;
    crate::tools::require("ffmpeg")?;
    crate::tools::require("ffprobe")?;
    let mut inputs = Vec::new();
    for case in &manifest.cases {
        let source = base.join(&case.source).canonicalize()?;
        let oracle = base.join(&case.oracle).canonicalize()?;
        ensure!(
            std::fs::metadata(&source)?.len() <= 16_777_216
                && std::fs::metadata(&oracle)?.len() <= 1_048_576,
            "reference exceeds compact size limit"
        );
        ensure!(
            crate::bundle::sha256_file(&source)?
                .0
                .eq_ignore_ascii_case(&case.source_sha256),
            "{} source SHA mismatch",
            case.id
        );
        ensure!(
            crate::bundle::sha256_file(&oracle)?
                .0
                .eq_ignore_ascii_case(&case.oracle_sha256),
            "{} oracle SHA mismatch",
            case.id
        );
        let text = std::fs::read_to_string(oracle)?;
        let probe = crate::media::probe(&source)?;
        let reference = match &case.expect {
            Expected::Audio {
                source_rate,
                source_channels,
                source_frames,
                ..
            } => {
                let audio = crate::media::streams(&probe, "audio");
                ensure!(
                    audio.len() == 1
                        && crate::media::streams(&probe, "video").is_empty()
                        && audio[0]["sample_rate"].as_str()
                            == Some(source_rate.to_string().as_str())
                        && audio[0]["channels"].as_u64() == Some(*source_channels as u64),
                    "{} audio source metadata disagrees with manifest",
                    case.id
                );
                let rows = events(&text)?;
                ensure!(
                    rows.iter()
                        .all(|e| e.channel < *source_channels && e.sample < *source_frames)
                        && (0..*source_channels).all(|c| rows.iter().any(|e| e.channel == c)),
                    "{} incomplete/out-of-range audio oracle",
                    case.id
                );
                Oracle::Audio(rows)
            }
            Expected::Video { .. } => {
                let video = crate::media::streams(&probe, "video");
                ensure!(
                    video.len() == 1
                        && crate::media::streams(&probe, "audio").is_empty()
                        && video[0]["width"] == 320
                        && video[0]["height"] == 180,
                    "{} barcode source must be video-only320x180",
                    case.id
                );
                Oracle::Video(frames(&text)?)
            }
        };
        inputs.push((source, probe, reference));
    }
    if args.validate_only {
        println!(
            "edit-reference: {} inputs validated; no product launched; manifest {}",
            manifest.cases.len(),
            manifest_path.display()
        );
        return Ok(ExitCode::SUCCESS);
    }
    let output_dir = args.out.context("--out is required")?;
    let product_dir = args.product.context("--product is required")?;
    ensure!(
        !output_dir.exists() || std::fs::read_dir(&output_dir)?.next().is_none(),
        "output directory must be fresh/empty"
    );
    std::fs::create_dir_all(&output_dir)?;
    let out = output_dir.canonicalize()?;
    std::fs::write(out.join("protocol.json"), &protocol)?;
    let caps = crate::capability::probe(&[]);
    let mut ctx = Context::new(
        Lane::Preflight,
        caps,
        None,
        Some(product_dir.canonicalize()?),
        out.clone(),
    )?;
    let product = ctx.product().map_err(|e| anyhow::anyhow!("{e:?}"))?;
    let (product_sha, _) = crate::bundle::sha256_file(&product.exe)?;
    let mut results = Vec::new();
    let write_report = |results: &Vec<Value>| -> Result<()> {
        std::fs::write(
            out.join("result.json"),
            serde_json::to_vec_pretty(
                &json!({"schema":"exosnap.edit-reference-result/1","protocolSha256":crate::bundle::sha256_bytes(protocol.as_bytes()),"product":product.exe,"productSha256":product_sha,"limits":"No native preview, physical endpoint, RS1 or full acceptance claim","cases":results}),
            )?,
        )?;
        Ok(())
    };
    let mut halted = None;
    for (case, (source, probe, oracle)) in manifest.cases.iter().zip(&inputs) {
        if let Some(reason) = halted {
            results.push(json!({"id":case.id,"verdict":"NOT_RUN","detail":reason}));
            write_report(&results)?;
            continue;
        }
        ctx.begin(&case.id)?;
        let started = Instant::now();
        let outcome = run_case(
            &mut ctx,
            case,
            source,
            probe,
            oracle,
            manifest.max_output_bytes,
        );
        let cleanup = ctx.job.terminate();
        let cleanup_failed = cleanup.is_err();
        let (verdict, detail) = match (outcome, cleanup) {
            (_, Err(e)) => ("INFRA_ERROR", format!("process cleanup: {e:#}")),
            (Ok(_), Ok(_)) => (
                "PASS",
                String::from("full-source export matched its declared bounded oracle"),
            ),
            (Err(Stop::Fail(e)), Ok(_)) => ("FAIL", e),
            (Err(Stop::Unavailable(e)), Ok(_)) => ("UNAVAILABLE", e),
            (Err(Stop::Infra(e)), Ok(_)) => ("INFRA_ERROR", format!("{e:#}")),
        };
        results.push(json!({"id":case.id,"verdict":verdict,"detail":detail,"durationMs":started.elapsed().as_millis(),"sourceSha256":case.source_sha256,"oracleSha256":case.oracle_sha256,"evidence":std::mem::take(&mut ctx.evidence.0)}));
        write_report(&results)?;
        if cleanup_failed {
            halted = Some("prior product process cleanup failed");
        }
        if held_bytes(&out)? > manifest.max_output_bytes {
            halted = Some("retained output envelope exhausted; remaining cases were not launched");
        }
    }
    let passed = results.iter().filter(|r| r["verdict"] == "PASS").count();
    println!(
        "edit-reference: {passed}/{} PASS; report {}",
        manifest.cases.len(),
        out.join("result.json").display()
    );
    Ok(if passed == manifest.cases.len() {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn audio() -> Expected {
        Expected::Audio {
            source_rate: 48000,
            source_channels: 1,
            source_frames: 192000,
            output_rate: 48000,
            output_channels: 2,
            channel_map: vec![0, 0],
            channel_gain: vec![1.0, 1.0],
            detector: Detector {
                window_samples: 32,
                peak_offset_samples: 2,
                sum_relative_tolerance: 0.1,
                peak_min_ratio: 0.3,
                peak_max_ratio: 1.2,
                quiet_pcm24: 256,
            },
        }
    }

    fn manifest() -> serde_json::Value {
        json!({"schema":"exosnap.edit-reference/1","max_output_bytes":33554432,"cases":[{
            "id":"mono","source":"mono.wav","source_sha256":"a".repeat(64),
            "oracle":"events.csv","oracle_sha256":"b".repeat(64),"expect":{
                "kind":"audio","source_rate":48000,"source_channels":1,"source_frames":192000,
                "output_rate":48000,"output_channels":2,"channel_map":[0,0],"channel_gain":[1,1],
                "detector":{"window_samples":32,"peak_offset_samples":2,"sum_relative_tolerance":0.1,
                    "peak_min_ratio":0.3,"peak_max_ratio":1.2,"quiet_pcm24":256}}}]})
    }

    #[test]
    fn strict_manifest_refuses_unsafe_ids_rates_mappings_and_unknown_keys() {
        assert!(parse_manifest(&manifest().to_string()).is_ok());
        for path in ["id", "rate", "map", "unknown", "budget", "duplicate"] {
            let mut input = manifest();
            match path {
                "id" => input["cases"][0]["id"] = json!("../escape"),
                "rate" => input["cases"][0]["expect"]["output_rate"] = json!(44100),
                "map" => input["cases"][0]["expect"]["channel_map"] = json!([1, 1]),
                "unknown" => input["cases"][0]["unexpected"] = json!(true),
                "budget" => input["max_output_bytes"] = json!(33554433u64),
                _ => {
                    let duplicate = input["cases"][0].clone();
                    input["cases"].as_array_mut().unwrap().push(duplicate);
                }
            }
            assert!(parse_manifest(&input.to_string()).is_err(), "{path}");
        }
    }

    fn pcm(events: &[Event]) -> Vec<u8> {
        let mut data = vec![0; 192000 * 2 * 3];
        for event in events {
            for channel in 0..2 {
                let at = (event.sample * 2 + channel) * 3;
                let bytes = (event.pcm16 * 256).to_le_bytes();
                data[at..at + 3].copy_from_slice(&bytes[..3]);
            }
        }
        data
    }

    #[test]
    fn exact_audio_checks_every_sample_and_explicit_mono_mapping() {
        let events = vec![Event {
            channel: 0,
            sample: 7,
            pcm16: -8192,
        }];
        let good = pcm(&events);
        assert!(judge_audio(&good, &events, &audio()).is_ok());
        let mut wrong = good.clone();
        wrong[7 * 6 + 3] = 1;
        assert!(judge_audio(&wrong, &events, &audio()).is_err());
        assert!(judge_audio(&good[..good.len() - 6], &events, &audio()).is_err());
    }

    #[test]
    fn video_checks_barcode_identity_and_pts_not_only_frame_count() {
        let expected = Expected::Video {
            output_fps_num: 30,
            output_fps_den: 1,
            pts_tolerance_us: 1000,
            barcode_threshold: 128,
            allowed_paths: vec!["Copy".into(), "Render".into()],
        };
        let frames = vec![
            Frame {
                id: 0,
                pts_us: 0,
                end_us: 20000,
            },
            Frame {
                id: 1,
                pts_us: 20000,
                end_us: 70000,
            },
        ];
        let mut bars = vec![0; 20];
        bars[10] = 255;
        assert!(judge_video(&bars, &[0, 20000], &frames, &expected, "Copy").is_ok());
        assert!(judge_video(&bars, &[0, 33000], &frames, &expected, "Copy").is_err());
        bars[10] = 0;
        assert!(judge_video(&bars, &[0, 20000], &frames, &expected, "Copy").is_err());
        assert!(judge_video(&bars, &[0, 20000], &frames, &expected, "Unknown").is_err());
        let mut rendered = vec![0; 30];
        rendered[10] = 255;
        rendered[20] = 255;
        assert!(judge_video(&rendered, &[0, 33333, 66667], &frames, &expected, "Render").is_ok());
    }

    #[test]
    fn resampled_audio_checks_signed_area_peak_time_and_unrelated_channels() {
        let mut expected = audio();
        if let Expected::Audio {
            source_rate,
            source_frames,
            ..
        } = &mut expected
        {
            *source_rate = 96000;
            *source_frames = 384000;
        }
        let events = vec![Event {
            channel: 0,
            sample: 96000,
            pcm16: 16384,
        }];
        let good = pcm(&[Event {
            channel: 0,
            sample: 48000,
            pcm16: 8192,
        }]);
        assert!(judge_audio(&good, &events, &expected).is_ok());
        let shifted = pcm(&[Event {
            channel: 0,
            sample: 48007,
            pcm16: 8192,
        }]);
        assert!(judge_audio(&shifted, &events, &expected).is_err());
        let wrong_area = pcm(&[Event {
            channel: 0,
            sample: 48000,
            pcm16: 4096,
        }]);
        assert!(judge_audio(&wrong_area, &events, &expected).is_err());
        let mut unrelated = good;
        unrelated[6 * 100 + 5] = 1;
        assert!(judge_audio(&unrelated, &events, &expected).is_err());
    }

    #[test]
    fn exact_audio_uses_the_pcm24_full_scale_quantizer() {
        let event = Event {
            channel: 0,
            sample: 7,
            pcm16: 24576,
        };
        let mut data = pcm(std::slice::from_ref(&event));
        for channel in 0..2 {
            let at = (7 * 2 + channel) * 3;
            data[at..at + 3].copy_from_slice(&6_291_455i32.to_le_bytes()[..3]);
        }
        assert!(judge_audio(&data, &[event], &audio()).is_ok());
    }

    #[test]
    fn external_oracles_reject_duplicate_events_and_overflowing_intervals() {
        assert!(events("channel,sample_index,pcm_s16\n0,7,8192\n0,7,8192\n").is_err());
        assert!(frames("frame_id,pts_ticks,duration_ticks,time_base_num,time_base_den,barcode_lsb_first\n0,9223372036854775807,1,9223372036854775807,1,0000000000\n").is_err());
    }

    #[test]
    fn copy_review_reserves_generated_pcm_and_refuses_unbounded_tracks() {
        let expected = Expected::Video {
            output_fps_num: 30,
            output_fps_den: 1,
            pts_tolerance_us: 1000,
            barcode_threshold: 128,
            allowed_paths: vec!["Copy".into()],
        };
        let oracle = Oracle::Video(vec![Frame {
            id: 0,
            pts_us: 0,
            end_us: 10_000_000,
        }]);
        let review = json!({"path":"Copy","outputAudioTracks":[{"codec":"PCM24","sampleRate":48000,"channels":2}]});
        let reserve = review_reserve(&review, 1000, &oracle, &expected).unwrap();
        assert!(reserve >= 1000 + 480000 * 2 * 3 + 1_048_576);
        let unknown = json!({"path":"Copy","outputAudioTracks":[{"codec":"AAC","sampleRate":48000,"channels":2}]});
        assert!(review_reserve(&unknown, 1000, &oracle, &expected).is_err());
        assert!(review_reserve(&json!({"path":"Copy"}), 1000, &oracle, &expected).is_err());
    }

    #[test]
    fn import_uses_full_editor_snapshot_clips_and_reports_settled_refusals() {
        let complete = json!({"importPending":false,"clips":[{"id":1}],"workspaceError":null});
        assert!(import_ready(&complete).unwrap());
        let pending = json!({"importPending":true,"clips":[],"workspaceError":null});
        assert!(!import_ready(&pending).unwrap());
        let refused =
            json!({"importPending":false,"clips":[],"workspaceError":"source admission refused"});
        assert!(
            import_ready(&refused)
                .unwrap_err()
                .to_string()
                .contains("source admission refused")
        );
        assert!(import_ready(&json!({"importPending":false,"clipCount":1})).is_err());
    }
}
