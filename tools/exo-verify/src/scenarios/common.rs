//! Shared scenario machinery: the stimulus process, the recording lifecycle
//! driven over the control endpoint, and the recording oracles.

use anyhow::{Context as _, anyhow};
use serde_json::{Value, json};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::time::{Duration, Instant};

use crate::context::{App, Context, json_path};
use crate::media::{self, IdTimeline};
use crate::scenario::{Step, Stop};
use crate::stimulus::{LogEvent, read_log};
use crate::{infra_ensure, product_ensure};

pub fn secs(s: f64) -> Duration {
    Duration::from_secs_f64(s)
}

// ---------------------------------------------------------------------------
// Stimulus
// ---------------------------------------------------------------------------

pub struct Stimulus {
    pub child: Child,
    stdin: ChildStdin,
    pub log_path: PathBuf,
    pub title: String,
    pub monitor: String,
    pub rect: [i32; 4],
    pub qpc_frequency: i64,
    pub dpi: u32,
}

#[derive(Default, Clone)]
pub struct StimulusOptions {
    pub fullscreen: bool,
    pub still: bool,
    pub cursor: Option<&'static str>,
    pub cursor_shape: Option<&'static str>,
    pub marker_interval: Option<f64>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub generation: Option<u8>,
}

/// The monitor release-gpu scenarios draw on: `EXO_VERIFY_MONITOR`, else the
/// primary display. A dedicated test machine sets nothing.
pub fn target_monitor() -> Option<String> {
    std::env::var("EXO_VERIFY_MONITOR")
        .ok()
        .filter(|s| !s.is_empty())
}

impl Stimulus {
    pub fn start(ctx: &mut Context, options: StimulusOptions) -> Step<Stimulus> {
        let nonce = crate::control::new_run_id("stim");
        let title = format!("ExoVerify Stimulus {nonce}");
        let log_path = ctx.scenario_dir.join("stimulus.jsonl");
        let mut command = Command::new(std::env::current_exe()?);
        command
            .arg("stimulus")
            .arg("--log")
            .arg(&log_path)
            .arg("--title")
            .arg(&title)
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        if let Some(m) = target_monitor() {
            command.args(["--monitor", &m]);
        }
        if options.fullscreen {
            command.arg("--fullscreen");
        }
        if options.still {
            command.arg("--still");
        }
        if let Some(c) = options.cursor {
            command.args(["--cursor", c]);
        }
        if let Some(c) = options.cursor_shape {
            command.args(["--cursor-shape", c]);
        }
        if let Some(i) = options.marker_interval {
            command.args(["--marker-interval", &i.to_string()]);
        }
        if let Some(w) = options.width {
            command.args(["--width", &w.to_string()]);
        }
        if let Some(h) = options.height {
            command.args(["--height", &h.to_string()]);
        }
        if let Some(g) = options.generation {
            command.args(["--generation", &g.to_string()]);
        }
        let mut child = ctx.spawn(&mut command)?;
        let stdin = child.stdin.take().context("stimulus stdin")?;
        let deadline = Instant::now() + secs(15.0);
        loop {
            if let Ok(events) = read_log(&log_path)
                && let Some(LogEvent::Ready {
                    monitor,
                    rect,
                    qpc_frequency,
                    dpi,
                    ..
                }) = events
                    .iter()
                    .find(|e| matches!(e, LogEvent::Ready { .. }))
                    .cloned()
            {
                ctx.evidence.put(
                    "stimulus",
                    json!({ "monitor": monitor, "rect": rect, "dpi": dpi, "title": title }),
                );
                return Ok(Stimulus {
                    child,
                    stdin,
                    log_path,
                    title,
                    monitor,
                    rect,
                    qpc_frequency,
                    dpi,
                });
            }
            if let Ok(Some(status)) = child.try_wait() {
                return Err(Stop::infra(format!(
                    "the stimulus exited during startup ({status})"
                )));
            }
            infra_ensure!(
                Instant::now() < deadline,
                "the stimulus window did not report ready within 15 s"
            );
            std::thread::sleep(Duration::from_millis(100));
        }
    }

    pub fn command(&mut self, cmd: &str) -> Step {
        writeln!(self.stdin, "{cmd}")
            .map_err(|e| Stop::infra(format!("stimulus command {cmd}: {e}")))?;
        self.stdin.flush()?;
        Ok(())
    }

    /// Waits until the stimulus logs `state`, the ground truth for lifecycle
    /// scenarios (a window really destroyed, really minimised).
    pub fn wait_state(&self, state: &str, timeout: Duration) -> Step<i64> {
        let deadline = Instant::now() + timeout;
        loop {
            for e in self.events()? {
                if let LogEvent::State { state: s, qpc } = e
                    && s == state
                {
                    return Ok(qpc);
                }
            }
            if Instant::now() > deadline {
                return Err(Stop::infra(format!(
                    "the stimulus never reported '{state}'"
                )));
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    pub fn events(&self) -> Step<Vec<LogEvent>> {
        Ok(read_log(&self.log_path)?)
    }

    pub fn frames(&self) -> Step<Vec<(u32, f64, bool)>> {
        let f = self.qpc_frequency as f64;
        Ok(self
            .events()?
            .into_iter()
            .filter_map(|e| match e {
                LogEvent::Frame { id, qpc, flash } => Some((id, qpc as f64 / f, flash)),
                _ => None,
            })
            .collect())
    }

    pub fn audio_available(&self) -> Step<bool> {
        Ok(!self
            .events()?
            .iter()
            .any(|e| matches!(e, LogEvent::AudioUnavailable { .. })))
    }

    /// Mean stimulus frame rate over its log.
    pub fn frame_rate(&self) -> Step<f64> {
        let frames = self.frames()?;
        infra_ensure!(
            frames.len() > 10,
            "the stimulus presented too few frames to measure its rate"
        );
        let span = frames.last().unwrap().1 - frames.first().unwrap().1;
        Ok((frames.len() - 1) as f64 / span.max(1e-6))
    }

    pub fn stop(mut self) {
        let _ = self.command("quit");
        let _ = crate::tools::wait(&mut self.child, secs(3.0));
        let _ = self.child.kill();
    }
}

// ---------------------------------------------------------------------------
// Product configuration and the recording lifecycle
// ---------------------------------------------------------------------------

/// Applies settings and returns what the product reconciled each to. A
/// request the product reconciled to something else is reported to the
/// caller, who decides whether that is a capability limit or a defect.
pub fn configure(app: &mut App, settings: &[(&str, Value)]) -> Step<Vec<(String, Value, Value)>> {
    let mut changed = Vec::new();
    for (key, value) in settings {
        let result = app
            .client
            .request(
                "settings.set",
                json!({ "key": key, "value": value }),
                secs(15.0),
            )?
            .map_err(|r| Stop::infra(format!("settings.set {key}={value} refused: {r}")))?;
        // The answer is the value read back through the same key, not an echo.
        let applied = result["values"].get(*key).cloned().unwrap_or(Value::Null);
        if applied != *value {
            changed.push((key.to_string(), value.clone(), applied));
        }
    }
    Ok(changed)
}

/// Like `configure`, but a reconciled video/audio codec request means this
/// machine cannot produce it: the scenario is UNAVAILABLE here.
pub fn configure_exact(app: &mut App, settings: &[(&str, Value)]) -> Step {
    let changed = configure(app, settings)?;
    if let Some((key, wanted, got)) = changed.first() {
        return Err(Stop::unavailable(format!(
            "the product reconciled {key} from {wanted} to {got} on this machine"
        )));
    }
    Ok(())
}

pub fn select_display(app: &mut App, device: &str) -> Step {
    let snapshot = app
        .client
        .request(
            "record.selectTarget",
            json!({ "kind": "monitor", "titleFilter": device }),
            secs(15.0),
        )?
        .map_err(|r| Stop::infra(format!("record.selectTarget monitor {device}: {r}")))?;
    infra_ensure!(
        snapshot["selectedTargetIndex"].as_i64().unwrap_or(-1) >= 0,
        "the product did not select display {device}"
    );
    Ok(())
}

pub fn select_window(app: &mut App, title: &str) -> Step {
    let deadline = Instant::now() + secs(10.0);
    loop {
        // The target list refreshes on its own cadence; a window created a
        // moment ago may not be enumerated yet.
        let answer = app.client.request(
            "record.selectTarget",
            json!({ "kind": "window", "titleFilter": title }),
            secs(15.0),
        )?;
        if let Ok(s) = &answer
            && s["selectedTargetIndex"].as_i64().unwrap_or(-1) >= 0
        {
            return Ok(());
        }
        if Instant::now() > deadline {
            return Err(Stop::infra(format!(
                "the product never offered the window '{title}' as a target"
            )));
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

pub fn record_snapshot(app: &mut App) -> Step<Value> {
    Ok(app.call("record.snapshot", json!({}))?)
}

fn wait_record(
    app: &mut App,
    what: &str,
    timeout: Duration,
    predicate: impl Fn(&Value) -> bool,
) -> Step<Value> {
    match app
        .client
        .poll("record.snapshot", json!({}), timeout, predicate)?
    {
        Some(v) => Ok(v),
        None => {
            let last = record_snapshot(app)?;
            Err(Stop::fail(format!(
                "{what} within {} s (state '{}', blocked {}, failed {})",
                timeout.as_secs(),
                last["stateText"].as_str().unwrap_or("?"),
                last["blocked"],
                last["failed"]
            )))
        }
    }
}

pub fn start_recording(app: &mut App) -> Step<Instant> {
    let answer = app.client.request("record.start", json!({}), secs(20.0))?;
    if let Err(r) = answer {
        return Err(Stop::fail(format!(
            "record.start was refused: {r} (requires {}, actual {})",
            r.requires, r.actual
        )));
    }
    wait_record(app, "the recording did not start", secs(20.0), |s| {
        s["recording"] == true
    })?;
    Ok(Instant::now())
}

pub fn pause(app: &mut App) -> Step {
    app.client
        .request("record.pause", json!({}), secs(10.0))?
        .map_err(|r| Stop::fail(format!("record.pause refused: {r}")))?;
    wait_record(app, "the recording did not pause", secs(10.0), |s| {
        s["paused"] == true
    })?;
    Ok(())
}

pub fn resume(app: &mut App) -> Step {
    app.client
        .request("record.resume", json!({}), secs(10.0))?
        .map_err(|r| Stop::fail(format!("record.resume refused: {r}")))?;
    wait_record(app, "the recording did not resume", secs(10.0), |s| {
        s["paused"] == false && s["recording"] == true
    })?;
    Ok(())
}

/// Stops and waits for a finished result. A result that is not a success is
/// a product failure with the product's own error phase and detail.
pub fn stop_recording(app: &mut App) -> Step<Value> {
    app.client
        .request("record.stop", json!({}), secs(20.0))?
        .map_err(|r| Stop::fail(format!("record.stop refused: {r}")))?;
    wait_record(app, "the recording did not finish", secs(120.0), |s| {
        s["recording"] == false && s["finalizing"] == false && s["preparing"] == false
    })?;
    let result = app.call("record.result", json!({}))?;
    product_ensure!(
        result["hasResult"] == true,
        "the product finished without a recording result"
    );
    product_ensure!(
        result["succeeded"] == true,
        "the recording failed: {} ({} {})",
        result["statusText"].as_str().unwrap_or(""),
        result["errorPhase"].as_str().unwrap_or(""),
        result["errorDetail"].as_str().unwrap_or("")
    );
    Ok(result)
}

pub fn output_path(result: &Value) -> Step<PathBuf> {
    let p = PathBuf::from(result["outputPath"].as_str().unwrap_or_default());
    product_ensure!(
        p.is_file(),
        "the reported output {} does not exist",
        p.display()
    );
    Ok(p)
}

/// Records for `seconds` of wall time. The duration is the measurement
/// window, not a settle delay.
pub fn record_for(app: &mut App, seconds: f64) -> Step<(Value, PathBuf, f64)> {
    let started = start_recording(app)?;
    std::thread::sleep(secs(seconds));
    let result = stop_recording(app)?;
    let wall = started.elapsed().as_secs_f64();
    let path = output_path(&result)?;
    Ok((result, path, wall))
}

// ---------------------------------------------------------------------------
// Oracles
// ---------------------------------------------------------------------------

pub struct VideoExpectation {
    pub codec: &'static str,
    pub fps: f64,
    /// Longest acceptable stretch of output time over which the stimulus id did not change.
    pub max_hold_s: f64,
    /// Minimum fraction of output frames that must show a new stimulus id.
    pub min_fresh_fraction: f64,
}

pub struct VideoFacts {
    pub probe: Value,
    pub timeline: IdTimeline,
    pub frames: Vec<media::LumaFrame>,
    pub duration: f64,
}

/// Container, packet and pixel checks common to every recording.
pub fn judge_video(
    ctx: &mut Context,
    file: &Path,
    stimulus: &Stimulus,
    expect: &VideoExpectation,
) -> Step<VideoFacts> {
    let probe = media::probe(file)?;
    let videos = media::streams(&probe, "video");
    product_ensure!(
        videos.len() == 1,
        "expected exactly one video stream, found {}",
        videos.len()
    );
    let v = videos[0];
    let codec = v["codec_name"].as_str().unwrap_or("");
    product_ensure!(
        codec == expect.codec,
        "video codec is {codec}, expected {}",
        expect.codec
    );
    let rate = media::rate(v["r_frame_rate"].as_str().unwrap_or("0/1")).unwrap_or(0.0);
    product_ensure!(
        (rate - expect.fps).abs() < 0.01,
        "stream frame rate is {rate}, expected {}",
        expect.fps
    );

    let (frames_n, packets_n) = media::frame_packet_counts(file)?;
    product_ensure!(
        frames_n == packets_n,
        "{packets_n} packets decode to {frames_n} frames"
    );

    let packets = media::video_packets(file)?;
    product_ensure!(
        packets.first().is_some_and(|p| p.key),
        "the first video packet is not a keyframe"
    );
    let step = 1.0 / expect.fps;
    let irregular = packets
        .windows(2)
        .filter(|w| ((w[1].pts - w[0].pts) - step).abs() > step * 0.5)
        .count();
    ctx.evidence.put("irregularPtsSteps", irregular);

    let frames = media::luma_frames(file, 480)?;
    let timeline = media::id_timeline(&frames);
    ctx.evidence
        .put("idTimeline", serde_json::to_value(&timeline)?);
    let duration =
        frames.last().map(|f| f.pts).unwrap_or(0.0) - frames.first().map(|f| f.pts).unwrap_or(0.0);

    product_ensure!(
        timeline.absent == 0,
        "{} of {} frames do not show the stimulus the product was told to capture",
        timeline.absent,
        timeline.frames
    );
    product_ensure!(
        timeline.corrupt * 50 <= timeline.frames,
        "{} of {} frames carry a damaged pattern",
        timeline.corrupt,
        timeline.frames
    );
    product_ensure!(
        timeline.reorders == 0,
        "frames were written out of order ({} reorders)",
        timeline.reorders
    );
    product_ensure!(
        timeline.longest_hold_s <= expect.max_hold_s,
        "the captured content stood still for {:.3} s although the stimulus kept changing",
        timeline.longest_hold_s
    );
    let fresh = timeline.unique as f64 / timeline.frames.max(1) as f64;
    let stimulus_rate = stimulus.frame_rate()?;
    // A stimulus slower than the output rate caps how fresh the output can be.
    let achievable = (stimulus_rate / expect.fps).min(1.0);
    ctx.evidence.put("freshFraction", fresh);
    ctx.evidence.put("stimulusRate", stimulus_rate);
    product_ensure!(
        fresh >= expect.min_fresh_fraction * achievable,
        "only {:.1}% of output frames show new content (achievable {:.1}%)",
        fresh * 100.0,
        achievable * 100.0
    );

    // Timeline fit: output PTS against the stimulus's own QPC for the same id.
    let truth: std::collections::HashMap<u32, f64> = stimulus
        .frames()?
        .into_iter()
        .map(|(id, t, _)| (id, t))
        .collect();
    let pairs: Vec<(f64, f64)> = timeline
        .samples
        .iter()
        .filter_map(|(pts, id)| truth.get(id).map(|t| (*t, *pts)))
        .collect();
    infra_ensure!(
        pairs.len() >= 20,
        "too few recorded frames matched the stimulus log to fit a timeline"
    );
    let (slope, _) = media::fit(&pairs).ok_or_else(|| Stop::infra("timeline fit failed"))?;
    ctx.evidence.put("ptsPerStimulusSecond", slope);
    if pairs.last().unwrap().0 - pairs[0].0 >= 8.0 {
        product_ensure!(
            (slope - 1.0).abs() <= 0.005,
            "recorded timestamps advance {slope:.4} s per real second"
        );
    }
    Ok(VideoFacts {
        probe,
        timeline,
        frames,
        duration,
    })
}

/// A/V alignment from the stimulus's synchronised flash + beep markers.
pub fn judge_av_sync(
    ctx: &mut Context,
    file: &Path,
    audio_stream: usize,
    video: &VideoFacts,
) -> Step {
    let samples = media::audio_samples(file, audio_stream, 48_000)?;
    let onsets = media::tone_onsets(&samples, 48_000, 1000.0, 0.5);
    let flashes: Vec<f64> = {
        let mut out = Vec::new();
        let mut last = false;
        for f in &video.frames {
            let on = crate::pattern::flash_on(&f.luma());
            if on && !last {
                out.push(f.pts);
            }
            last = on;
        }
        out
    };
    ctx.evidence.put("audioOnsets", json!(onsets));
    ctx.evidence.put("videoFlashes", json!(flashes));
    infra_ensure!(
        flashes.len() >= 3,
        "fewer than three flash markers were recorded; nothing to align"
    );
    product_ensure!(
        onsets.len() + 1 >= flashes.len(),
        "{} flash markers but only {} beeps in the audio track",
        flashes.len(),
        onsets.len()
    );
    let offsets: Vec<f64> = flashes
        .iter()
        .filter_map(|v| {
            onsets
                .iter()
                .map(|a| a - v)
                .min_by(|a, b| a.abs().total_cmp(&b.abs()))
        })
        .filter(|o| o.abs() < 1.0)
        .collect();
    product_ensure!(offsets.len() >= 3, "beeps could not be paired with flashes");
    let mut sorted = offsets.clone();
    sorted.sort_by(f64::total_cmp);
    let median = sorted[sorted.len() / 2];
    let spread = sorted.last().unwrap() - sorted.first().unwrap();
    ctx.evidence.put("avOffsetMedianMs", median * 1000.0);
    ctx.evidence.put("avOffsetSpreadMs", spread * 1000.0);
    // The render-side latency of the beep and the display latency of the flash
    // are both unmeasured here, so the offset is a bounded estimate, not an
    // exact point. The spread across markers is what reveals drift.
    product_ensure!(
        median.abs() <= 0.12,
        "audio is {:.0} ms away from video",
        median * 1000.0
    );
    product_ensure!(
        spread <= 0.06,
        "A/V offset moved by {:.0} ms during the recording",
        spread * 1000.0
    );
    Ok(())
}

pub fn audio_streams(probe: &Value) -> usize {
    media::streams(probe, "audio").len()
}

pub fn field<'a>(v: &'a Value, path: &str) -> Step<&'a Value> {
    json_path(v, path)
        .ok_or_else(|| Stop::fail(format!("the control surface no longer reports '{path}'")))
}
