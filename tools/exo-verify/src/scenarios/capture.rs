//! Window capture stall and minimized-window quiet behavior.

use serde_json::{Value, json};
use std::time::{Duration, Instant};

use super::common::{self, Stimulus, StimulusOptions, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::media;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "capture.window-stall-notice",
            revision: 1,
            title: "A visible stalled window raises an honest standing notice",
            claim: "a fullscreen-shaped window that stops presenting frames produces a stall notice while recording continues, without claiming unmeasured exclusive fullscreen",
            lane: Lane::Gpu,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
                Capability::Ffprobe,
            ],
            timeout: secs(180.0),
            run: window_stall,
        },
        // Required: minimizing the captured window is ordinary use, and a
        // recording that ends on it loses the user's work. No other scenario
        // exercises the minimized geometry WGC reports.
        Scenario {
            id: "capture.minimized-window-quiet",
            revision: 2,
            title: "A minimized quiet window keeps recording without a stall notice",
            claim: "a minimized window with a measured halt in captured frames stays free of a new stall notice while recording continues, resumes capturing when restored, and finalizes",
            lane: Lane::Gpu,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
                Capability::Ffprobe,
            ],
            timeout: secs(180.0),
            run: minimized_quiet,
        },
    ]
}

fn configure(app: &mut App) -> Step {
    common::configure_exact(
        app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.frameRate", json!(30)),
            ("audio.systemEnabled", json!(false)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
        ],
    )
}

fn notifications(app: &mut App) -> Step<Value> {
    Ok(app.call("notifications.snapshot", json!({}))?)
}

fn max_sequence(snapshot: &Value) -> Step<f64> {
    let entries = snapshot["entries"]
        .as_array()
        .ok_or_else(|| Stop::infra("notifications.snapshot has no entries"))?;
    Ok(entries
        .iter()
        .filter_map(|entry| entry["sequence"].as_f64())
        .fold(0.0_f64, f64::max))
}

fn fresh_stall_notice(snapshot: &Value, baseline: f64) -> Step<Option<String>> {
    let entries = snapshot["entries"]
        .as_array()
        .ok_or_else(|| Stop::infra("notifications.snapshot has no entries"))?;
    Ok(entries
        .iter()
        .filter(|entry| {
            entry["sequence"]
                .as_f64()
                .is_some_and(|sequence| sequence > baseline)
        })
        .filter_map(|entry| {
            let title = entry["title"].as_str()?;
            let lower = title.to_ascii_lowercase();
            (lower.contains("stall") || lower.contains("no frame"))
                .then(|| format!("{title} {}", entry["body"].as_str().unwrap_or("")))
        })
        .next())
}

fn judge_stillness(first: &Value, second: &Value) -> Step {
    let a = first["capture"]["framesCaptured"]
        .as_f64()
        .ok_or_else(|| Stop::infra("first pipeline sample has no capture.framesCaptured"))?;
    let b = second["capture"]["framesCaptured"]
        .as_f64()
        .ok_or_else(|| Stop::infra("second pipeline sample has no capture.framesCaptured"))?;
    infra_ensure!(b >= a, "captured frame counter went backward ({a} to {b})");
    if b != a {
        return Err(Stop::unavailable(format!(
            "capture kept producing frames ({a} to {b}); a stall was not established"
        )));
    }
    Ok(())
}

fn judge_active_capture(snapshot: &Value) -> Step {
    infra_ensure!(
        snapshot["capture"]["framesCaptured"]
            .as_f64()
            .is_some_and(|count| count > 0.0),
        "the window capture produced no frames before the quiet interval"
    );
    Ok(())
}

fn judge_running(pipeline: &Value) -> Step {
    product_ensure!(
        matches!(pipeline["lifecycle"].as_str(), Some("recording" | "paused")),
        "recording left the running lifecycle during a quiet window: {}",
        pipeline["lifecycle"]
    );
    Ok(())
}

fn judge_stall(notice: &str, pipeline: &Value) -> Step {
    product_ensure!(
        !notice.is_empty(),
        "no standing capture-stall notice appeared"
    );
    judge_running(pipeline)?;
    let claims_exclusive = {
        let lower = notice.to_ascii_lowercase();
        lower.contains("exclusive") || lower.contains("fullscreen")
    };
    product_ensure!(
        !claims_exclusive || pipeline["sourcePresentation"]["presentMode"] == "exclusiveFullscreen",
        "stall notice claims exclusive fullscreen without a measured exclusiveFullscreen present mode"
    );
    Ok(())
}

fn judge_quiet(notice: Option<&str>, pipeline: &Value) -> Step {
    product_ensure!(
        notice.is_none(),
        "a minimized window raised a stall notice: {}",
        notice.unwrap_or("")
    );
    judge_running(pipeline)
}

fn judge_resumed(quiet: &Value, restored: &Value) -> Step {
    judge_running(restored)?;
    let before = quiet["capture"]["framesCaptured"]
        .as_f64()
        .ok_or_else(|| Stop::infra("the quiet pipeline sample has no capture.framesCaptured"))?;
    let after = restored["capture"]["framesCaptured"]
        .as_f64()
        .ok_or_else(|| Stop::infra("the restored pipeline sample has no capture.framesCaptured"))?;
    product_ensure!(
        after > before,
        "capture did not resume after the window was restored ({before} to {after} frames)"
    );
    Ok(())
}

fn finalized_video(app: &mut App) -> Step {
    let result = common::stop_recording(app)?;
    let output = common::output_path(&result)?;
    let probe = media::probe(&output)?;
    product_ensure!(
        media::streams(&probe, "video").len() == 1,
        "the quiet-window recording has no video stream"
    );
    let (frames, _) = media::frame_packet_counts(&output)?;
    product_ensure!(
        frames > 0,
        "the quiet-window recording has no decoded frames"
    );
    Ok(())
}

fn observe(ctx: &mut Context, frozen: bool) -> Step {
    let mut app = ctx.launch(&[])?;
    configure(&mut app)?;
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            fullscreen: frozen,
            ..Default::default()
        },
    )?;
    common::select_window(&mut app, &stimulus.title)?;
    let baseline = max_sequence(&notifications(&mut app)?)?;
    common::start_recording(&mut app)?;
    std::thread::sleep(secs(3.0));
    let active = app.call("pipeline.snapshot", json!({}))?;
    judge_active_capture(&active)?;
    ctx.evidence.put("pipelineBeforeSilence", active);
    if frozen {
        stimulus.command("freeze")?;
        stimulus.wait_state("frozen", secs(5.0))?;
    } else {
        stimulus.command("minimize")?;
        stimulus.wait_state("minimized", secs(5.0))?;
    }
    std::thread::sleep(secs(13.0));
    let first = app.call("pipeline.snapshot", json!({}))?;
    std::thread::sleep(secs(4.0));
    let second = app.call("pipeline.snapshot", json!({}))?;
    ctx.evidence.put("pipelineFirst", first.clone());
    ctx.evidence.put("pipelineSecond", second.clone());
    judge_stillness(&first, &second)?;
    let notice = if frozen {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            let snapshot = notifications(&mut app)?;
            if let Some(text) = fresh_stall_notice(&snapshot, baseline)? {
                ctx.evidence.put("notifications", snapshot);
                break Some(text);
            }
            if Instant::now() >= deadline {
                ctx.evidence.put("notifications", snapshot);
                break None;
            }
            std::thread::sleep(Duration::from_millis(250));
        }
    } else {
        let snapshot = notifications(&mut app)?;
        let notice = fresh_stall_notice(&snapshot, baseline)?;
        ctx.evidence.put("notifications", snapshot);
        notice
    };
    if frozen {
        judge_stall(notice.as_deref().unwrap_or(""), &second)?;
    } else {
        judge_quiet(notice.as_deref(), &second)?;
        stimulus.command("restore")?;
        stimulus.wait_state("restored", secs(5.0))?;
        std::thread::sleep(secs(3.0));
        let restored = app.call("pipeline.snapshot", json!({}))?;
        ctx.evidence.put("pipelineRestored", restored.clone());
        judge_resumed(&second, &restored)?;
    }
    finalized_video(&mut app)?;
    stimulus.stop();
    Ok(())
}

fn window_stall(ctx: &mut Context) -> Step {
    observe(ctx, true)
}

fn minimized_quiet(ctx: &mut Context) -> Step {
    observe(ctx, false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn stillness_requires_equal_measured_frame_counts() {
        assert!(matches!(
            judge_stillness(
                &json!({"capture": {"framesCaptured": 8}}),
                &json!({"capture": {"framesCaptured": 9}})
            ),
            Err(Stop::Unavailable(_))
        ));
        judge_stillness(
            &json!({"capture": {"framesCaptured": 8}}),
            &json!({"capture": {"framesCaptured": 8}}),
        )
        .unwrap();
    }

    #[test]
    fn stall_notice_cannot_guess_exclusive_fullscreen() {
        let pipeline = json!({"lifecycle": "recording", "sourcePresentation": {"presentMode": "composedFlip", "modeAvailability": "available"}});
        assert!(matches!(
            judge_stall("Exclusive fullscreen may be blocking capture", &pipeline),
            Err(Stop::Fail(_))
        ));
        judge_stall("Window capture appears stalled", &pipeline).unwrap();
    }

    #[test]
    fn quiet_window_rejects_a_fresh_stall_notice() {
        assert!(matches!(
            judge_quiet(
                Some("Window capture stalled"),
                &json!({"lifecycle": "recording"})
            ),
            Err(Stop::Fail(_))
        ));
        judge_quiet(None, &json!({"lifecycle": "recording"})).unwrap();
    }

    #[test]
    fn a_restored_window_must_resume_a_running_capture() {
        let quiet = json!({"lifecycle": "recording", "capture": {"framesCaptured": 90}});
        judge_resumed(
            &quiet,
            &json!({"lifecycle": "recording", "capture": {"framesCaptured": 150}}),
        )
        .unwrap();
        assert!(matches!(
            judge_resumed(
                &quiet,
                &json!({"lifecycle": "recording", "capture": {"framesCaptured": 90}})
            ),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_resumed(
                &quiet,
                &json!({"lifecycle": "failed", "capture": {"framesCaptured": 150}})
            ),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn stall_probe_requires_capture_to_have_started_before_silence() {
        assert!(matches!(
            judge_active_capture(&json!({"capture": {"framesCaptured": 0}})),
            Err(Stop::Infra(_))
        ));
        judge_active_capture(&json!({"capture": {"framesCaptured": 42}})).unwrap();
    }
}
