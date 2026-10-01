//! Webcam overlay verification.
//!
//! Hardware-backed: the scenario requires a probed physical camera and reports
//! UNAVAILABLE without one, so ordinary CI without a webcam never fails on it.
//! Placement and mirroring are configured through the product's own settings
//! and live overlay commands; the encoded output is judged by frame change,
//! not by looking for a face.

use serde_json::json;

use super::common::{self, Stimulus, StimulusOptions, secs};
use crate::capability::Capability;
use crate::context::Context;
use crate::media;
use crate::plan::Tier;
use crate::product_ensure;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "webcam.overlay-live",
        revision: 1,
        title: "A physical webcam keeps updating a still capture and re-enables cleanly",
        class: ScenarioClass::Hardware,
        contract: "during a live recording on a stimulus display, an enabled physical camera advances the composited webcam generation independently of the source, the applied overlay rectangle is read back exactly, the recording finalizes, and disabling then re-enabling the camera round-trips",
        lane: Lane::Gpu,
        also: &[Lane::Preflight],
        tier: Tier::Recommended,
        requires: &[
            Capability::InteractiveDesktop,
            Capability::Wgc,
            Capability::Nvenc,
            Capability::Webcam,
            Capability::Ffprobe,
            Capability::Ffmpeg,
        ],
        timeout: secs(300.0),
        run: webcam_overlay_live,
    }]
}

fn webcam_overlay_live(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.frameRate", json!(30)),
            ("video.cfr", json!(true)),
            ("audio.systemEnabled", json!(false)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
            ("app.hideWindowFromCapture", json!(true)),
            ("webcam.enabled", json!(true)),
        ],
    )?;
    // The stimulus keeps presenting, because a fully quiet desktop can stop
    // delivering capture frames at all; the webcam oracle is the product's own
    // webcam-generation counter, which is independent of the source content.
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            fullscreen: true,
            marker_interval: Some(1.0),
            ..Default::default()
        },
    )?;
    common::select_display(&mut app, &stimulus.monitor)?;
    let selected = common::record_snapshot(&mut app)?;
    product_ensure!(
        selected["webcamSource"] == "device",
        "the run does not use the physical camera (webcamSource {:?})",
        selected["webcamSource"]
    );

    common::start_recording(&mut app)?;
    std::thread::sleep(secs(2.0));
    let applied = app
        .client
        .request(
            "webcam.overlay.set",
            json!({"x": 0.05, "y": 0.06, "width": 0.32, "height": 0.32, "opacity": 1.0, "mirror": true}),
            secs(15.0),
        )?
        .map_err(|refusal| Stop::fail(format!("webcam.overlay.set was refused during a recording: {refusal}")))?;
    product_ensure!(
        applied["applied"]["enabled"] == true,
        "the webcam overlay did not enable during the recording"
    );
    for (field, expected) in [("x", 0.05), ("y", 0.06), ("width", 0.32), ("height", 0.32)] {
        let actual = applied["applied"][field].as_f64().unwrap_or(f64::NAN);
        product_ensure!(
            (actual - expected).abs() < 1e-3,
            "the applied webcam {field} is {actual}, not {expected}"
        );
    }
    let first = app.call("pipeline.snapshot", json!({}))?;
    std::thread::sleep(secs(3.0));
    let second = app.call("pipeline.snapshot", json!({}))?;
    let generations = |snapshot: &serde_json::Value| {
        snapshot["retainedFrames"]["webcamGenerationChanges"]
            .as_u64()
            .ok_or_else(|| {
                Stop::fail(
                    "pipeline.snapshot no longer reports retainedFrames.webcamGenerationChanges",
                )
            })
    };
    let before = generations(&first)?;
    let after = generations(&second)?;
    let advanced = after.saturating_sub(before);
    product_ensure!(
        advanced >= 10,
        "the camera advanced {advanced} generations in 3 s; it is not updating"
    );

    let result = common::stop_recording(&mut app)?;
    let file = common::output_path(&result)?;
    let probe = media::probe(&file)?;
    product_ensure!(
        media::streams(&probe, "video").len() == 1,
        "the webcam recording has no single video stream"
    );
    let frames = media::luma_frames(&file, 320)?;
    product_ensure!(
        frames.len() >= 30,
        "the webcam recording decoded only {} frames",
        frames.len()
    );
    let changing = frames
        .windows(2)
        .filter(|pair| pair[0].data != pair[1].data)
        .count();
    product_ensure!(
        changing * 5 >= frames.len(),
        "the webcam recording is nearly static ({changing} of {} frame pairs changed)",
        frames.len() - 1
    );

    // Disable/re-enable is a settings round trip; the camera must come back.
    common::configure_exact(&mut app, &[("webcam.enabled", json!(false))])?;
    let disabled = app.call("settings.get", json!({"key": "webcam.enabled"}))?;
    common::configure_exact(&mut app, &[("webcam.enabled", json!(true))])?;
    let reenabled = app.call("settings.get", json!({"key": "webcam.enabled"}))?;
    product_ensure!(
        disabled["values"]["webcam.enabled"] == false
            && reenabled["values"]["webcam.enabled"] == true,
        "the camera setting did not round-trip: disabled {disabled}, re-enabled {reenabled}"
    );

    ctx.evidence.put("appliedOverlay", applied);
    ctx.evidence
        .put("webcamGenerationsInThreeSeconds", json!(advanced));
    ctx.evidence.put("webcamChangingPairs", json!(changing));
    ctx.evidence.put("webcamDisabled", disabled);
    ctx.evidence.put("webcamReenabled", reenabled);
    ctx.evidence.put("webcamResult", result);
    stimulus.stop();
    Ok(())
}
