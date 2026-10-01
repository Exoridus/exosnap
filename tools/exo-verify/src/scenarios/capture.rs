//! Window capture behavior, region geometry and the region selector gesture.
//!
//! The capture correctness scenarios drive the product through semantic IPC and
//! judge decoded output against the stimulus's own log. Only the region
//! selector scenario synthesises pointer input, because drag hit-testing,
//! commit and Escape are the product under test there.

use serde_json::{Value, json};
use std::time::{Duration, Instant};

use super::common::{self, Stimulus, StimulusOptions, VideoExpectation, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::media;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "capture.window-stall-notice",
            revision: 1,
            title: "A visible stalled window raises an honest standing notice",
            class: ScenarioClass::Contract,
            contract: "a fullscreen-shaped window that stops presenting frames produces a stall notice while recording continues, without claiming unmeasured exclusive fullscreen",
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
            class: ScenarioClass::Contract,
            contract: "a minimized window with a measured halt in captured frames stays free of a new stall notice while recording continues, resumes capturing when restored, and finalizes",
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
        // The move/geometry flow the throwaway cmd stimulus used to prove by
        // hand: a real WGC window recording follows the window, and a resize
        // ends the session with the product's documented size-change answer
        // rather than keeping stale geometry.
        Scenario {
            id: "capture.window-geometry-live",
            revision: 1,
            title: "A moving window keeps recording and a resize ends it honestly",
            class: ScenarioClass::Contract,
            contract: "WGC records a moving window without losing the changing stimulus, and a client-size change never silently keeps stale geometry: the session either encodes the new size or ends with the structured size-change reason",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(300.0),
            run: window_geometry_live,
        },
        // Region geometry without the selector gesture: the capture contract,
        // judged from the decoded output, not from a pointer drag.
        Scenario {
            id: "capture.region-contract",
            revision: 1,
            title: "A semantic region records exactly its requested pixels",
            class: ScenarioClass::Contract,
            contract: "record.selectRegion applies an exact physical-pixel rectangle to a display and the finished recording has exactly those dimensions with changing source content",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(240.0),
            run: region_contract,
        },
        // The gesture itself: the real selector overlay turns a pointer drag
        // into the region the product reports, and Escape leaves no region.
        // Desktop-interactive by nature; classified in the GPU lane because it
        // needs only a normal interactive desktop, not a physical rig.
        Scenario {
            id: "capture.region-selector-interaction",
            revision: 1,
            title: "The region selector commits a real drag and Escape restores the target",
            class: ScenarioClass::Contract,
            contract: "the real selector overlay turns a pointer drag into the region the product reports within two pixels, and Escape cancels without leaving a region or changing the previous target",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::DxgiDuplication,
                Capability::Wgc,
            ],
            timeout: secs(180.0),
            run: region_selector_interaction,
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

// ---------------------------------------------------------------------------
// Window geometry, region contract and the region selector gesture
// ---------------------------------------------------------------------------

fn pipeline_frames(snapshot: &Value) -> Step<u64> {
    snapshot["capture"]["framesCaptured"]
        .as_f64()
        .map(|value| value as u64)
        .ok_or_else(|| Stop::infra("the pipeline sample has no capture.framesCaptured"))
}

/// The product's answer to a client-size change during a window recording:
/// a finished result (live resize supported) or a structured failure, both
/// read from the product rather than assumed.
fn wait_resize_outcome(app: &mut App) -> Step<Value> {
    let deadline = Instant::now() + secs(20.0);
    loop {
        let snapshot = common::record_snapshot(app)?;
        let idle = snapshot["recording"] == false
            && snapshot["preparing"] == false
            && snapshot["finalizing"] == false;
        if snapshot["failed"] == true || idle {
            let result = app.call("record.result", json!({}))?;
            if result["hasResult"] == true {
                return Ok(result);
            }
        }
        if Instant::now() > deadline {
            // No failure arrived, so the session is still running: a product
            // that supports a live resize must now encode the new size.
            return common::stop_recording(app);
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

fn window_geometry_live(ctx: &mut Context) -> Step {
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
        ],
    )?;
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            width: Some(960),
            height: Some(540),
            marker_interval: Some(1.0),
            ..Default::default()
        },
    )?;
    let client = stimulus.client_size()?;
    common::select_window(&mut app, &stimulus.title)?;
    let selected = common::record_snapshot(&mut app)?;
    product_ensure!(
        selected["sourceKindText"] == "WINDOW",
        "the product selected {:?} instead of the stimulus window",
        selected["sourceKindText"]
    );
    product_ensure!(
        selected["sourceName"] == stimulus.title,
        "the product names the window {:?}, not {:?}",
        selected["sourceName"],
        stimulus.title
    );

    // Move half: a recording that stays live while the window relocates.
    common::start_recording(&mut app)?;
    std::thread::sleep(secs(2.0));
    let before = app.call("pipeline.snapshot", json!({}))?;
    let moved_rect = stimulus.move_window()?;
    std::thread::sleep(secs(2.5));
    let after = app.call("pipeline.snapshot", json!({}))?;
    product_ensure!(
        matches!(after["lifecycle"].as_str(), Some("recording" | "paused")),
        "the recording left the running lifecycle when the window moved: {}",
        after["lifecycle"]
    );
    let advanced = pipeline_frames(&after)?.saturating_sub(pipeline_frames(&before)?);
    product_ensure!(
        advanced > 0,
        "capture produced no frames after the window moved"
    );
    let result = common::stop_recording(&mut app)?;
    let file = common::output_path(&result)?;
    let video = common::judge_video(
        ctx,
        &file,
        &stimulus,
        &VideoExpectation {
            codec: "h264",
            fps: 30.0,
            max_hold_s: 0.3,
            min_fresh_fraction: 0.4,
        },
    )?;
    ctx.evidence.put("stimulusClientAtStart", json!(client));
    ctx.evidence.put("stimulusRectAfterMove", json!(moved_rect));
    ctx.evidence.put("framesAfterMove", json!(advanced));
    ctx.evidence
        .put("movedVideoDurationSeconds", json!(video.duration));
    ctx.evidence.put("movedRecordResult", result);

    // Resize half: a geometry change must never be silently ignored.
    common::start_recording(&mut app)?;
    std::thread::sleep(secs(2.0));
    let (resized_rect, resized_client) = stimulus.resize_window()?;
    let outcome = wait_resize_outcome(&mut app)?;
    if outcome["succeeded"] == true {
        product_ensure!(
            outcome["outputWidth"] == resized_client[0]
                && outcome["outputHeight"] == resized_client[1],
            "the session survived a resize but encoded {}x{}, not the new client {}x{}",
            outcome["outputWidth"],
            outcome["outputHeight"],
            resized_client[0],
            resized_client[1]
        );
        ctx.evidence.put("resizeOutcome", "encoded-new-client-size");
    } else {
        let detail = outcome["errorDetail"]
            .as_str()
            .unwrap_or("")
            .to_ascii_lowercase();
        product_ensure!(
            detail.contains("size") || detail.contains("dimension") || detail.contains("geometry"),
            "the session failed after a resize without naming the geometry change: {}",
            outcome["errorDetail"]
        );
        ctx.evidence
            .put("resizeOutcome", "ended-with-structured-size-change");
    }
    ctx.evidence
        .put("stimulusRectAfterResize", json!(resized_rect));
    ctx.evidence.put("resizeResult", outcome);
    stimulus.stop();
    Ok(())
}

/// Whether the product's `formatText` headline names exactly this pixel size.
fn region_format_matches(text: &Value, width: i32, height: i32, tolerance: i32) -> bool {
    let Some(text) = text.as_str() else {
        return false;
    };
    let Some((w, rest)) = text.split_once('\u{d7}') else {
        return false;
    };
    let Some(h) = rest.split_whitespace().next() else {
        return false;
    };
    let (Ok(w), Ok(h)) = (w.trim().parse::<i32>(), h.parse::<i32>()) else {
        return false;
    };
    (w - width).abs() <= tolerance && (h - height).abs() <= tolerance
}

fn region_contract(ctx: &mut Context) -> Step {
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
        ],
    )?;
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            fullscreen: true,
            marker_interval: Some(1.0),
            ..Default::default()
        },
    )?;
    let monitor_w = stimulus.rect[2] - stimulus.rect[0];
    let monitor_h = stimulus.rect[3] - stimulus.rect[1];
    infra_ensure!(
        monitor_w >= 256 && monitor_h >= 256,
        "the stimulus monitor is too small to crop a region"
    );
    // The right half, top to middle: the barcode's low-order bits there flip
    // every stimulus frame and the flash square lives at x 85..97%, so the
    // recorded crop changes continuously. The left half's high bits can hold
    // still for seconds at a time.
    let region_x = monitor_w / 2;
    let region_y = 0;
    let region_w = monitor_w / 2;
    let region_h = monitor_h / 2;
    app.client
        .request(
            "record.selectRegion",
            json!({"display": stimulus.monitor, "x": region_x, "y": region_y, "width": region_w,
                   "height": region_h}),
            secs(20.0),
        )?
        .map_err(|refusal| Stop::infra(format!("record.selectRegion was refused: {refusal}")))?;
    let selected = common::record_snapshot(&mut app)?;
    product_ensure!(
        selected["sourceKindText"] == "REGION",
        "the product reports {:?}, not REGION",
        selected["sourceKindText"]
    );
    product_ensure!(
        region_format_matches(&selected["formatText"], region_w, region_h, 0),
        "the product reports {:?} for the {region_w}x{region_h} region",
        selected["formatText"]
    );
    ctx.evidence.put(
        "requestedRegion",
        json!([region_x, region_y, region_w, region_h]),
    );
    ctx.evidence.put("recordSnapshotBeforeStart", selected);

    let (result, file, wall) = common::record_for(&mut app, 6.0)?;
    product_ensure!(
        result["outputWidth"] == region_w && result["outputHeight"] == region_h,
        "the finished recording reports {}x{}, not the requested {region_w}x{region_h}",
        result["outputWidth"],
        result["outputHeight"]
    );
    let probe = media::probe(&file)?;
    let videos = media::streams(&probe, "video");
    product_ensure!(
        videos.len() == 1,
        "the region recording has {} video streams, not one",
        videos.len()
    );
    product_ensure!(
        videos[0]["width"] == region_w && videos[0]["height"] == region_h,
        "the encoded stream is {}x{}, not {region_w}x{region_h}",
        videos[0]["width"],
        videos[0]["height"]
    );
    let frames = media::luma_frames(&file, 320)?;
    product_ensure!(
        frames.len() >= 30,
        "the region recording decoded only {} frames",
        frames.len()
    );
    let changing = frames
        .windows(2)
        .filter(|pair| pair[0].data != pair[1].data)
        .count();
    product_ensure!(
        changing * 2 >= frames.len(),
        "the region recording is static ({changing} of {} frame pairs changed)",
        frames.len() - 1
    );
    ctx.evidence.put("regionDecodedFrames", json!(frames.len()));
    ctx.evidence.put("regionChangingPairs", json!(changing));
    ctx.evidence.put("regionWallSeconds", json!(wall));
    ctx.evidence.put("regionResult", result);
    stimulus.stop();
    Ok(())
}

/// Opens the real selector overlay and gives its window a moment to appear.
fn open_region_selector(app: &mut App) -> Step {
    app.client
        .request("record.openRegionSelector", json!({}), secs(15.0))?
        .map_err(|refusal| {
            Stop::infra(format!("record.openRegionSelector was refused: {refusal}"))
        })?;
    std::thread::sleep(secs(0.6));
    Ok(())
}

/// A screen's physical-pixel rectangle on the virtual desktop. Qt geometry is
/// device-independent, so it is scaled by the screen's own ratio; the selector
/// scenario is a single-DPI, single-screen check by contract.
fn physical_screen_rect(screen: &Value) -> Step<(i32, i32, i32, i32)> {
    let number = |key: &str| {
        screen[key]
            .as_f64()
            .ok_or_else(|| Stop::infra(format!("environment screen has no {key}")))
    };
    let dpr = screen["devicePixelRatio"].as_f64().unwrap_or(1.0);
    let (x, y, width, height) = (
        number("x")?,
        number("y")?,
        number("width")?,
        number("height")?,
    );
    Ok((
        (x * dpr) as i32,
        (y * dpr) as i32,
        (width * dpr) as i32,
        (height * dpr) as i32,
    ))
}

fn region_selector_interaction(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let primary = common::primary_screen(&environment, false)
        .ok_or_else(|| Stop::unavailable("the product reports no primary display"))?;
    let device = common::screen_device(primary)?;
    common::select_display(&mut app, &device)?;
    let before = common::record_snapshot(&mut app)?;
    product_ensure!(
        before["sourceKindText"] == "SCREEN",
        "the display target reads {:?}, not SCREEN",
        before["sourceKindText"]
    );
    let previous_identity = before["selectedTargetIdentity"].clone();
    let (origin_x, origin_y, width, height) = physical_screen_rect(primary)?;

    // Commit flow: the real overlay turns a pointer drag into the product's
    // committed region. The commit click lands on the overlay's own
    // "Use selected region" button at the screen's bottom-right inset.
    let start = (origin_x + width / 4, origin_y + height / 4);
    let end = (origin_x + width * 3 / 4, origin_y + height * 3 / 4);
    open_region_selector(&mut app)?;
    crate::pointer::drag_rect(start, end, Duration::from_millis(40))
        .map_err(|error| Stop::infra(format!("pointer drag failed: {error:#}")))?;
    let commit = (origin_x + width - 40, origin_y + height - 40);
    crate::pointer::click(commit, Duration::from_millis(60))
        .map_err(|error| Stop::infra(format!("commit click failed: {error:#}")))?;
    let committed = app
        .client
        .poll("record.snapshot", json!({}), secs(10.0), |snapshot| {
            snapshot["sourceKindText"] == "REGION" && snapshot["sourceName"] == "Screen region"
        })?
        .ok_or_else(|| Stop::fail("the pointer drag produced no committed region"))?;
    let drag_w = end.0 - start.0;
    let drag_h = end.1 - start.1;
    product_ensure!(
        region_format_matches(&committed["formatText"], drag_w, drag_h, 2),
        "the committed region reports {:?}, not the dragged {drag_w}x{drag_h}",
        committed["formatText"]
    );
    ctx.evidence
        .put("draggedRect", json!([start.0, start.1, drag_w, drag_h]));
    ctx.evidence.put("committedRegionSnapshot", committed);

    // Cancel flow: open again, modify the tentative selection, Escape. The
    // previous target must come back and no region may remain.
    open_region_selector(&mut app)?;
    crate::pointer::drag_rect(
        (origin_x + width / 3, origin_y + height / 3),
        (origin_x + width / 2, origin_y + height / 2),
        Duration::from_millis(30),
    )
    .map_err(|error| Stop::infra(format!("pointer drag failed: {error:#}")))?;
    crate::pointer::press_key(0x1B, Duration::from_millis(80))
        .map_err(|error| Stop::infra(format!("Escape key failed: {error:#}")))?;
    let restored = app
        .client
        .poll("record.snapshot", json!({}), secs(10.0), |snapshot| {
            snapshot["sourceKindText"] == "SCREEN"
        })?
        .ok_or_else(|| Stop::fail("Escape did not restore the previous capture target"))?;
    product_ensure!(
        restored["selectedTargetIdentity"] == previous_identity,
        "Escape left target {} instead of restoring {}",
        restored["selectedTargetIdentity"],
        previous_identity
    );
    ctx.evidence.put("restoredAfterEscape", restored);
    Ok(())
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
