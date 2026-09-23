//! Long-running audio integrity observations.

use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::path::Path;
use std::process::Command;
use std::time::Duration;

use crate::capability::Capability;
use crate::context::{App, Context};
use crate::media;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::tools;
use crate::{infra_ensure, product_ensure};

use super::common::{self, secs};

const SOAK_SECONDS: f64 = 1800.0;

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "audio.mixed-clock-soak",
            revision: 1,
            title: "A 30 minute system and microphone recording stays complete",
            claim: "separate render and microphone tracks span a long recording with bounded duration skew and a clean session report",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::AudioRender,
                Capability::AudioCapture,
                Capability::Ffprobe,
            ],
            timeout: secs(1960.0),
            run: mixed_clock_soak,
        },
        Scenario {
            id: "audio.endpoint-degrade",
            revision: 1,
            title: "A lost audio endpoint degrades and recovers",
            claim: "disconnecting the bound physical playback device degrades then recovers the source without stopping the recording",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::AudioRender,
                Capability::AudioCapture,
                Capability::PhysicalAudioDisconnect,
                Capability::Operator,
                Capability::Ffprobe,
            ],
            timeout: secs(180.0),
            run: endpoint_degrade,
        },
        Scenario {
            id: "audio.connected-silence",
            revision: 1,
            title: "A connected silent endpoint is not degraded",
            claim: "a known silent but connected render endpoint remains active without source degradation",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::AudioRender,
                Capability::AudioCapture,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(120.0),
            run: connected_silence,
        },
        Scenario {
            id: "audio.endpoint-44100",
            revision: 1,
            title: "A 44.1 kHz endpoint records correct audio",
            claim: "a measured 44.1 kHz default render endpoint produces an expected audio rate and retains its original format and default role",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::AudioRender,
                Capability::AudioCapture,
                Capability::Ffprobe,
            ],
            timeout: secs(180.0),
            run: endpoint_44100,
        },
    ]
}

fn envctl_path() -> Step<std::path::PathBuf> {
    let path = std::env::var_os("EXO_VERIFY_ENVCTL")
        .or_else(|| std::env::var_os("EXOSNAP_ENVCTL"))
        .map(std::path::PathBuf::from)
        .or_else(|| tools::resolve("exosnap-envctl"))
        .ok_or_else(|| {
            Stop::unavailable("exosnap-envctl is not configured (set EXO_VERIFY_ENVCTL)")
        })?;
    if !path.is_file() {
        return Err(Stop::unavailable(format!(
            "envctl executable does not exist: {}",
            path.display()
        )));
    }
    Ok(path)
}

fn raw_endpoint_snapshot(alias: &str) -> Step<Value> {
    let output = tools::run(
        Command::new(envctl_path()?).args(["snapshot", "--aliases", alias]),
        secs(30.0),
    )?;
    infra_ensure!(
        output.success(),
        "envctl snapshot failed: {}",
        output.stderr.trim()
    );
    Ok(serde_json::from_str(&output.stdout)?)
}

fn endpoint_properties(alias: &str) -> Step<BTreeMap<String, String>> {
    let snapshot = raw_endpoint_snapshot(alias)?;
    let items = snapshot["properties"]
        .as_array()
        .ok_or_else(|| Stop::infra("envctl snapshot has no properties"))?;
    if items.is_empty() {
        return Err(Stop::unavailable(format!(
            "audio endpoint alias {alias} is not bound"
        )));
    }
    let mut values = BTreeMap::new();
    for item in items {
        infra_ensure!(
            item["ok"] == true,
            "envctl cannot read {}: {}",
            item["key"],
            item["error"]
        );
        let key = item["property"]
            .as_str()
            .ok_or_else(|| Stop::infra("envctl property lacks a name"))?;
        let value = item["value"]
            .as_str()
            .ok_or_else(|| Stop::infra("envctl property lacks a value"))?;
        values.insert(key.to_string(), value.to_string());
    }
    Ok(values)
}

fn prepared_endpoint(props: &BTreeMap<String, String>, rate: Option<u32>) -> Step {
    if props.get("endpoint-state").map(String::as_str) != Some("active") {
        return Err(Stop::unavailable("the bound render endpoint is not active"));
    }
    if !props
        .get("default-roles")
        .is_some_and(|roles| roles.split(',').any(|role| role == "console"))
    {
        return Err(Stop::unavailable(
            "the bound render endpoint is not the default console endpoint",
        ));
    }
    if let Some(rate) = rate {
        if !props
            .get("device-format")
            .is_some_and(|format| format.starts_with(&format!("{rate}/")))
        {
            return Err(Stop::unavailable(format!(
                "the bound render endpoint is not configured for {rate} Hz"
            )));
        }
    }
    Ok(())
}

fn recorded_rate_ok(rates: &[u32]) -> bool {
    !rates.is_empty() && rates.iter().all(|rate| matches!(rate, 44_100 | 48_000))
}

fn prepared_app(ctx: &mut Context) -> Step<App> {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.audioCodec", json!("Opus")),
            ("audio.systemEnabled", json!(true)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
        ],
    )?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let screen = environment["displays"]["screens"]
        .as_array()
        .and_then(|items| {
            items
                .iter()
                .find(|screen| screen["primary"] == true)
                .or_else(|| items.first())
        })
        .and_then(|screen| screen["name"].as_str())
        .ok_or_else(|| Stop::infra("product reported no primary display"))?;
    common::select_display(&mut app, screen)?;
    infra_ensure!(
        common::record_snapshot(&mut app)?["systemAudioEnabled"] == true,
        "the system-audio source did not become enabled"
    );
    Ok(app)
}

fn endpoint_44100(ctx: &mut Context) -> Step {
    let alias = std::env::var("EXO_VERIFY_44100_AUDIO_ALIAS")
        .unwrap_or_else(|_| "audio.render.44100-test".into());
    let before = endpoint_properties(&alias)?;
    prepared_endpoint(&before, Some(44_100))?;
    let mut app = prepared_app(ctx)?;
    let (result, file, wall) = common::record_for(&mut app, 10.0)?;
    let probe = media::probe(&file)?;
    let streams = media::streams(&probe, "audio");
    let rates: Vec<u32> = streams
        .iter()
        .filter_map(|stream| {
            stream["sample_rate"]
                .as_str()
                .and_then(|rate| rate.parse().ok())
        })
        .collect();
    product_ensure!(
        rates.len() == streams.len(),
        "an audio stream has no readable sample rate"
    );
    product_ensure!(
        recorded_rate_ok(&rates),
        "audio stream rates {rates:?} are neither 44100 nor 48000 Hz"
    );
    let duration = media::f64_field(&probe["format"], "duration")
        .ok_or_else(|| Stop::infra("ffprobe reported no container duration"))?;
    let spans = audio_packet_spans(&file, streams.len())?;
    product_ensure!(
        spans.iter().all(|span| *span >= duration * 0.99),
        "a 44.1 kHz endpoint audio track ends before the container"
    );
    let after = endpoint_properties(&alias)?;
    infra_ensure!(
        before == after,
        "the prepared 44.1 kHz endpoint changed during recording and was not restored"
    );
    ctx.evidence.put("audioEndpointBefore", json!(before));
    ctx.evidence.put("audioEndpointAfter", json!(after));
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("ffprobe", probe);
    ctx.evidence.put("audioPacketSpansSeconds", json!(spans));
    ctx.evidence.put("recordWallSeconds", wall);
    app.close()?;
    Ok(())
}

fn connected_silence(ctx: &mut Context) -> Step {
    let alias = std::env::var("EXO_VERIFY_SILENT_AUDIO_ALIAS")
        .map_err(|_| Stop::unavailable("set EXO_VERIFY_SILENT_AUDIO_ALIAS to a bound, operator-prepared silent render endpoint"))?;
    let before = endpoint_properties(&alias)?;
    prepared_endpoint(&before, None)?;
    let mut app = prepared_app(ctx)?;
    common::start_recording(&mut app)?;
    let mut samples = Vec::new();
    for _ in 0..30 {
        std::thread::sleep(Duration::from_millis(500));
        let pipeline = app.call("pipeline.snapshot", json!({}))?;
        product_ensure!(
            pipeline["lifecycle"] == "recording",
            "the recording stopped while the silent endpoint was observed"
        );
        samples.push(pipeline["audio"].clone());
    }
    let result = common::stop_recording(&mut app)?;
    let file = common::output_path(&result)?;
    let probe = media::probe(&file)?;
    product_ensure!(
        !media::streams(&probe, "audio").is_empty(),
        "the silent connected source produced no audio track"
    );
    infra_ensure!(
        samples.iter().any(|sample| sample["active"] == true),
        "the system-audio source was never active"
    );
    product_ensure!(
        samples
            .iter()
            .all(|sample| sample["sourceDegraded"] == false),
        "a connected silent source was reported degraded"
    );
    let decoded = media::audio_samples(&file, 0, 48_000)?;
    product_ensure!(
        decoded.len() >= 48_000 * 10,
        "the silent-source audio track decoded fewer than 10 seconds of samples"
    );
    let peak = media::peak(&decoded);
    let after = endpoint_properties(&alias)?;
    infra_ensure!(
        before == after,
        "the prepared silent endpoint or default roles changed during recording"
    );
    if peak > 0.001 {
        return Err(Stop::unavailable(format!(
            "the prepared endpoint was not silent (decoded peak {peak:.4})"
        )));
    }
    ctx.evidence.put("audioEndpointBefore", json!(before));
    ctx.evidence.put("audioEndpointAfter", json!(after));
    ctx.evidence.put("pipelineAudioSamples", json!(samples));
    ctx.evidence.put("audioPeak", peak as f64);
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("ffprobe", probe);
    app.close()?;
    Ok(())
}

fn endpoint_state(alias: &str) -> Step<String> {
    let snapshot = raw_endpoint_snapshot(alias)?;
    let state = snapshot["properties"]
        .as_array()
        .and_then(|items| {
            items
                .iter()
                .find(|item| item["property"] == "endpoint-state")
        })
        .ok_or_else(|| {
            Stop::infra("envctl has no endpoint-state property for the bound audio alias")
        })?;
    Ok(if state["ok"] == true {
        state["value"].as_str().unwrap_or("unknown").to_string()
    } else {
        "unavailable".to_string()
    })
}

fn wait_degradation(app: &mut App, wanted: bool, timeout: Duration) -> Step<bool> {
    let deadline = std::time::Instant::now() + timeout;
    while std::time::Instant::now() < deadline {
        let pipeline = app.call("pipeline.snapshot", json!({}))?;
        product_ensure!(
            pipeline["lifecycle"] == "recording",
            "recording stopped during audio endpoint loss or recovery"
        );
        if pipeline["audio"]["sourceDegraded"].as_bool() == Some(wanted) {
            return Ok(true);
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    Ok(false)
}

fn endpoint_degrade(ctx: &mut Context) -> Step {
    let alias = std::env::var("EXO_VERIFY_PHYSICAL_AUDIO_ALIAS")
        .unwrap_or_else(|_| "audio.render.normal".into());
    let before = endpoint_properties(&alias)?;
    prepared_endpoint(&before, None)?;
    let mut app = prepared_app(ctx)?;
    common::start_recording(&mut app)?;
    if !ctx.ask(&format!("Disconnect the physical render endpoint bound to {alias}, keep it disconnected, then answer yes."))? {
        let _ = common::stop_recording(&mut app);
        return Err(Stop::unavailable("operator did not disconnect the bound physical audio endpoint"));
    }
    let lost_state = endpoint_state(&alias);
    let degraded = wait_degradation(&mut app, true, secs(15.0));
    let restore_confirmed = ctx.ask(&format!(
        "Reconnect the same physical render endpoint bound to {alias}, then answer yes."
    ))?;
    let after = endpoint_properties(&alias);
    let recovered = wait_degradation(&mut app, false, secs(15.0));
    let result = common::stop_recording(&mut app)?;
    let file = common::output_path(&result)?;
    let probe = media::probe(&file)?;
    infra_ensure!(
        restore_confirmed,
        "operator did not confirm physical endpoint reconnection"
    );
    let lost_state = lost_state?;
    if lost_state == "active" {
        return Err(Stop::unavailable(
            "the bound audio endpoint still read active after the disconnect prompt",
        ));
    }
    let after = after?;
    infra_ensure!(
        before == after,
        "the physical endpoint, format or default roles did not return to their original values"
    );
    product_ensure!(
        degraded?,
        "the product never reported audio source degradation during the observed endpoint loss"
    );
    product_ensure!(
        recovered?,
        "the product never cleared audio source degradation after the endpoint returned"
    );
    product_ensure!(
        !media::streams(&probe, "audio").is_empty(),
        "the recovered recording has no audio track"
    );
    ctx.evidence.put("audioEndpointBefore", json!(before));
    ctx.evidence.put("audioEndpointLostState", lost_state);
    ctx.evidence.put("audioEndpointAfter", json!(after));
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("ffprobe", probe);
    app.close()?;
    Ok(())
}

fn mixed_clock_soak(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.audioCodec", json!("Opus")),
            ("audio.systemEnabled", json!(true)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(true)),
        ],
    )?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let inputs = environment["audio"]["inputs"]
        .as_array()
        .ok_or_else(|| Stop::infra("environment snapshot has no audio input list"))?;
    if !inputs.iter().any(|input| input["default"] == true) {
        return Err(Stop::unavailable(
            "no default microphone endpoint is available for the second clock",
        ));
    }
    let screen = environment["displays"]["screens"]
        .as_array()
        .and_then(|screens| {
            screens
                .iter()
                .find(|screen| screen["primary"] == true)
                .or_else(|| screens.first())
        })
        .and_then(|screen| screen["name"].as_str())
        .ok_or_else(|| Stop::infra("no primary display name was reported"))?;
    common::select_display(&mut app, screen)?;
    let enabled = common::record_snapshot(&mut app)?;
    infra_ensure!(
        enabled["systemAudioEnabled"] == true && enabled["microphoneEnabled"] == true,
        "both system and microphone sources did not become enabled"
    );
    let settings = app.call("settings.snapshot", json!({}))?;
    mixed_sources(&settings)?;
    let started = common::start_recording(&mut app)?;
    let mut drift_samples = Vec::new();
    while started.elapsed().as_secs_f64() < SOAK_SECONDS {
        let remain = SOAK_SECONDS - started.elapsed().as_secs_f64();
        std::thread::sleep(secs(remain.min(30.0)));
        let pipeline = app.call("pipeline.snapshot", json!({}))?;
        let recording = common::record_snapshot(&mut app)?;
        product_ensure!(
            recording["recording"] == true,
            "recording stopped during the 30 minute soak"
        );
        drift_samples.push(json!({
            "elapsedSeconds": started.elapsed().as_secs_f64(),
            "avDriftMs": pipeline["avTiming"]["avDriftMs"],
            "avDriftAvailability": pipeline["avTiming"]["avDriftAvailability"]
        }));
    }
    let result = common::stop_recording(&mut app)?;
    let wall = started.elapsed().as_secs_f64();
    let file = common::output_path(&result)?;
    let probe = media::probe(&file)?;
    let duration = media::f64_field(&probe["format"], "duration")
        .ok_or_else(|| Stop::infra("ffprobe reported no container duration"))?;
    let audio_count = media::streams(&probe, "audio").len();
    product_ensure!(
        audio_count == 2,
        "the soak recording has {audio_count} audio streams instead of separate system and microphone tracks"
    );
    let spans = audio_packet_spans(&file, audio_count)?;
    let report = app.call("session.latest", json!({}))?;
    mixed_track_count(&report, audio_count)?;
    ctx.evidence.put("audioSourceSettings", settings);
    ctx.evidence
        .put("audioEnvironment", environment["audio"].clone());
    ctx.evidence.put("driftSamples", json!(drift_samples));
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("session", report.clone());
    ctx.evidence.put("ffprobe", probe);
    ctx.evidence.put("audioPacketSpansSeconds", json!(spans));
    ctx.evidence.put("recordWallSeconds", wall);
    judge_soak(&report, duration, wall, &spans)
}

fn mixed_sources(settings: &Value) -> Step {
    let rows = settings["effective"]["audio"]["rows"]
        .as_array()
        .ok_or_else(|| Stop::infra("settings snapshot has no effective audio source rows"))?;
    for source in ["sys", "mic"] {
        let row = rows
            .iter()
            .find(|row| row["source"] == source)
            .ok_or_else(|| {
                Stop::infra(format!("settings snapshot lacks the {source} source row"))
            })?;
        if row["enabled"] != true || row["mergeWithAbove"] != false {
            return Err(Stop::unavailable(format!(
                "the effective {source} source is disabled or merged; two separate tracks are required"
            )));
        }
    }
    Ok(())
}

fn mixed_track_count(envelope: &Value, stream_count: usize) -> Step {
    let report = envelope.get("report").unwrap_or(envelope);
    let count = report["audio"]["track_count"]
        .as_u64()
        .ok_or_else(|| Stop::infra("session report has no audio track count"))?;
    let drains = report["audio"]["resampler_drain"]
        .as_array()
        .ok_or_else(|| Stop::infra("session report has no per-track resampler drain records"))?;
    product_ensure!(
        count == 2 && stream_count == 2 && drains.len() == 2,
        "session reported {count} audio tracks and {} drains but ffprobe found {stream_count}; two separate tracks are required",
        drains.len()
    );
    Ok(())
}

fn audio_packet_spans(file: &Path, stream_count: usize) -> Step<Vec<f64>> {
    let out = tools::run(
        Command::new(tools::require("ffprobe")?)
            .args([
                "-v",
                "error",
                "-select_streams",
                "a",
                "-show_entries",
                "packet=stream_index,pts_time,duration_time",
                "-of",
                "csv=p=0",
            ])
            .arg(file),
        Duration::from_secs(300),
    )?;
    infra_ensure!(
        out.success(),
        "ffprobe audio packet inspection failed: {}",
        out.stderr.trim()
    );
    let mut spans: Vec<(f64, f64)> = Vec::new();
    for line in out.stdout.lines() {
        let fields: Vec<_> = line.split(',').collect();
        infra_ensure!(
            fields.len() >= 3,
            "ffprobe returned a malformed audio packet row"
        );
        let index: usize = fields[0]
            .parse()
            .map_err(|_| Stop::infra("ffprobe returned a malformed audio stream index"))?;
        let pts: f64 = fields[1]
            .parse()
            .map_err(|_| Stop::infra("ffprobe returned a malformed audio packet timestamp"))?;
        let duration: f64 = fields[2]
            .parse()
            .map_err(|_| Stop::infra("ffprobe returned a malformed audio packet duration"))?;
        infra_ensure!(
            pts.is_finite() && duration.is_finite() && duration >= 0.0,
            "ffprobe returned a nonfinite audio packet time"
        );
        if spans.len() <= index {
            spans.resize(index + 1, (f64::INFINITY, f64::NEG_INFINITY));
        }
        spans[index].0 = spans[index].0.min(pts);
        spans[index].1 = spans[index].1.max(pts + duration);
    }
    infra_ensure!(
        spans
            .iter()
            .filter(|(first, last)| first.is_finite() && last.is_finite())
            .count()
            == stream_count,
        "ffprobe found packets for fewer audio streams than it reported"
    );
    Ok(spans
        .into_iter()
        .filter(|(first, _)| first.is_finite())
        .map(|(first, last)| last - first)
        .collect())
}

fn report_number(report: &Value, path: &str) -> Option<f64> {
    path.split('.')
        .try_fold(report, |value, key| value.get(key))
        .and_then(Value::as_f64)
}

fn judge_soak(
    envelope: &Value,
    container_seconds: f64,
    expected_seconds: f64,
    audio_spans: &[f64],
) -> Step {
    let report = envelope.get("report").unwrap_or(envelope);
    let counters = &report["counters"];
    infra_ensure!(
        counters.is_object(),
        "no session report counters were returned"
    );
    let required = [
        "mux_failures",
        "encoder_keyframe_prediction_mismatches",
        "frames_dropped.processing_failure",
        "frames_dropped.backpressure",
        "audio_discontinuity_ms_total",
        "audio_discontinuity_ms_longest",
        "audio_discontinuities",
    ];
    for key in required {
        infra_ensure!(
            report_number(counters, key).is_some(),
            "session report lacks counters.{key}"
        );
    }
    infra_ensure!(
        !audio_spans.is_empty(),
        "the audio track has no packet span"
    );
    product_ensure!(
        container_seconds.is_finite() && container_seconds > 0.0,
        "invalid container duration {container_seconds}"
    );
    product_ensure!(
        (container_seconds - expected_seconds).abs() <= expected_seconds * 0.02,
        "container {container_seconds:.1} s differs from {expected_seconds:.1} s recorded by more than 2%"
    );
    for span in audio_spans {
        product_ensure!(
            *span >= container_seconds * 0.99,
            "audio spans {span:.1} s of a {container_seconds:.1} s container"
        );
    }
    infra_ensure!(
        report["audio"]["degraded_occurred"].as_bool().is_some(),
        "the report has no audio degradation state"
    );
    product_ensure!(
        report["audio"]["degraded_occurred"] == false,
        "the audio source degraded during the soak"
    );
    let drains = report["audio"]["resampler_drain"]
        .as_array()
        .ok_or_else(|| Stop::infra("the report has no resampler drain records"))?;
    for drain in drains {
        infra_ensure!(
            drain["undrained_frames"].as_f64().is_some(),
            "a resampler drain record lacks undrained_frames"
        );
        product_ensure!(
            drain["undrained_frames"].as_f64() == Some(0.0),
            "a resampler left undrained frames"
        );
    }
    let segments = report["segments"]
        .as_array()
        .ok_or_else(|| Stop::infra("the report has no segments"))?;
    infra_ensure!(
        !segments.is_empty(),
        "the report has no finalized segment to judge"
    );
    product_ensure!(
        segments.iter().all(|segment| segment["finalized"] == true),
        "a recording segment was not finalized"
    );
    for key in [
        "mux_failures",
        "encoder_keyframe_prediction_mismatches",
        "frames_dropped.processing_failure",
        "frames_dropped.backpressure",
    ] {
        product_ensure!(
            report_number(counters, key) == Some(0.0),
            "counters.{key} is nonzero"
        );
    }
    let total = report_number(counters, "audio_discontinuity_ms_total").unwrap();
    let longest = report_number(counters, "audio_discontinuity_ms_longest").unwrap();
    product_ensure!(
        total <= expected_seconds,
        "audio discontinuities total {total:.0} ms, over the 0.1% budget"
    );
    product_ensure!(
        longest <= 120.0,
        "the longest audio discontinuity is {longest:.0} ms"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn report() -> Value {
        json!({"report":{"counters":{
            "mux_failures":0,
            "encoder_keyframe_prediction_mismatches":0,
            "frames_dropped":{"processing_failure":0,"backpressure":0},
            "audio_discontinuity_ms_total":20,
            "audio_discontinuity_ms_longest":10,
            "audio_discontinuities":1
        },"audio":{"degraded_occurred":false,"resampler_drain":[{"track":"system","undrained_frames":0}]},
        "segments":[{"index":0,"finalized":true}]}})
    }

    #[test]
    fn soak_requires_all_report_counters() {
        let mut incomplete = report();
        incomplete["report"]["counters"]
            .as_object_mut()
            .unwrap()
            .remove("mux_failures");
        assert!(matches!(
            judge_soak(&incomplete, 1800.0, 1800.0, &[1799.0]),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn soak_rejects_a_short_audio_track_and_undrained_tail() {
        assert!(matches!(
            judge_soak(&report(), 1800.0, 1800.0, &[1700.0]),
            Err(Stop::Fail(_))
        ));
        let mut undrained = report();
        undrained["report"]["audio"]["resampler_drain"][0]["undrained_frames"] = json!(12);
        assert!(matches!(
            judge_soak(&undrained, 1800.0, 1800.0, &[1799.0]),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn soak_accepts_a_complete_track_and_clean_report() {
        judge_soak(&report(), 1800.0, 1800.0, &[1799.0]).unwrap();
    }

    #[test]
    fn mixed_clock_soak_requires_two_separate_enabled_sources_and_tracks() {
        let settings = json!({"effective":{"audio":{"rows":[
            {"source":"sys","enabled":true,"mergeWithAbove":false},
            {"source":"mic","enabled":true,"mergeWithAbove":false}
        ]}}});
        mixed_sources(&settings).unwrap();
        let mut merged = settings.clone();
        merged["effective"]["audio"]["rows"][1]["mergeWithAbove"] = json!(true);
        assert!(matches!(mixed_sources(&merged), Err(Stop::Unavailable(_))));
        let mut disabled = settings.clone();
        disabled["effective"]["audio"]["rows"][1]["enabled"] = json!(false);
        assert!(matches!(
            mixed_sources(&disabled),
            Err(Stop::Unavailable(_))
        ));
        let mut two_track_report = report();
        two_track_report["report"]["audio"]["track_count"] = json!(2);
        two_track_report["report"]["audio"]["resampler_drain"] = json!([
            {"track":0,"undrained_frames":0},
            {"track":1,"undrained_frames":0}
        ]);
        assert!(mixed_track_count(&two_track_report, 2).is_ok());
        assert!(matches!(
            mixed_track_count(&two_track_report, 1),
            Err(Stop::Fail(_))
        ));
        two_track_report["report"]["audio"]["track_count"] = json!(1);
        assert!(matches!(
            mixed_track_count(&two_track_report, 2),
            Err(Stop::Fail(_))
        ));
        two_track_report["report"]["audio"]["track_count"] = json!(2);
        two_track_report["report"]["audio"]["resampler_drain"] = json!([]);
        assert!(matches!(
            mixed_track_count(&two_track_report, 2),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn soak_does_not_pass_without_audio_state() {
        let mut missing = report();
        missing["report"].as_object_mut().unwrap().remove("audio");
        assert!(matches!(
            judge_soak(&missing, 1800.0, 1800.0, &[1799.0]),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn physical_audio_gates_remain_in_plan() {
        let ids: Vec<_> = scenarios()
            .into_iter()
            .map(|scenario| scenario.id)
            .collect();
        for id in [
            "audio.endpoint-degrade",
            "audio.connected-silence",
            "audio.endpoint-44100",
        ] {
            assert!(ids.contains(&id));
        }
    }

    #[test]
    fn prepared_endpoint_requires_active_default_and_expected_format() {
        let props = std::collections::BTreeMap::from([
            ("endpoint-state".to_string(), "active".to_string()),
            (
                "default-roles".to_string(),
                "console,multimedia".to_string(),
            ),
            ("device-format".to_string(), "44100/16/2".to_string()),
        ]);
        prepared_endpoint(&props, Some(44100)).unwrap();
        assert!(matches!(
            prepared_endpoint(&props, Some(48000)),
            Err(Stop::Unavailable(_))
        ));
        let mut disconnected = props.clone();
        disconnected.insert("endpoint-state".into(), "unplugged".into());
        assert!(matches!(
            prepared_endpoint(&disconnected, Some(44100)),
            Err(Stop::Unavailable(_))
        ));
    }

    #[test]
    fn recorded_rate_must_be_endpoint_or_engine_mix_rate() {
        assert!(recorded_rate_ok(&[44100, 48000]));
        assert!(!recorded_rate_ok(&[32000]));
        assert!(!recorded_rate_ok(&[]));
    }
}
