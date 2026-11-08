//! Recording scenarios judged from the stimulus log and decoded output.

use serde_json::json;

use super::common::{self, Stimulus, StimulusOptions, VideoExpectation, secs};
use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "record.ddx-h264-mkv",
            revision: 2,
            title: "A 60 fps display recording decodes with aligned system audio",
            class: ScenarioClass::Hardware,
            contract: "DXGI display capture produces H.264 in MKV with a changing stimulus, ordered frames, decoded system audio and bounded A/V offset",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::DxgiDuplication,
                Capability::Nvenc,
                Capability::AudioRender,
                Capability::AudioCapture,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(240.0),
            run: ddx_h264_mkv,
        },
        Scenario {
            id: "record.wgc-hevc-mp4",
            revision: 1,
            title: "A 30 fps window recording decodes as HEVC in MP4",
            class: ScenarioClass::Contract,
            contract: "WGC captures the changing window stimulus in a decodable HEVC MP4 with ordered frames and bounded holds",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(180.0),
            run: wgc_hevc_mp4,
        },
        Scenario {
            id: "record.edit-export",
            revision: 2,
            title: "A paused recording passes through Review and Edit with the same exported frame timeline",
            class: ScenarioClass::Contract,
            contract: "an isolated WGC recording pauses and resumes, plays and seeks in Recording Review, reveals its registered identity in Edit, adds it from History, exports after review, and preserves decoded frame identities and timestamps",
            lane: Lane::Preflight,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::D3d11,
                Capability::Nvenc,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(360.0),
            run: recording_edit_export,
        },
        Scenario {
            id: "record.pause-resume",
            revision: 1,
            title: "Pause removes time and frames from a finished recording",
            class: ScenarioClass::Contract,
            contract: "pausing a WGC recording suppresses the changing stimulus during the pause, resumes afterward and excludes paused wall time from the decoded file",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(180.0),
            run: pause_resume,
        },
    ]
}

fn ddx_h264_mkv(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.audioCodec", json!("Opus")),
            ("video.frameRate", json!(60)),
            ("video.cfr", json!(true)),
            ("audio.systemEnabled", json!(true)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
            ("app.hideWindowFromCapture", json!(true)),
        ],
    )?;
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            fullscreen: true,
            marker_interval: Some(2.0),
            ..Default::default()
        },
    )?;
    if !stimulus.audio_available()? {
        return Err(Stop::unavailable(
            "the stimulus could not schedule WASAPI beeps",
        ));
    }
    common::select_display(&mut app, &stimulus.monitor)?;
    let (result, file, wall) = common::record_for(&mut app, 12.0)?;
    stimulus.stop();
    let video = common::judge_video(
        ctx,
        &file,
        &stimulus,
        &VideoExpectation {
            codec: "h264",
            fps: 60.0,
            max_hold_s: 0.25,
            min_fresh_fraction: 0.5,
        },
    )?;
    product_ensure!(
        video.probe["format"]["format_name"]
            .as_str()
            .is_some_and(|name| name.contains("matroska")),
        "the recording is not a Matroska file"
    );
    product_ensure!(
        common::audio_streams(&video.probe) == 1,
        "expected one system-audio stream, found {}",
        common::audio_streams(&video.probe)
    );
    common::judge_av_sync(ctx, &file, 0, &video)?;
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("recordWallSeconds", wall);
    ctx.evidence.put("videoDurationSeconds", video.duration);
    Ok(())
}

fn wgc_hevc_mp4(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MP4")),
            ("video.videoCodec", json!("HEVC")),
            ("video.audioCodec", json!("AAC")),
            ("video.frameRate", json!(30)),
            ("video.cfr", json!(true)),
        ],
    )?;
    common::disable_recording_audio(&mut app)?;
    let mut stimulus = Stimulus::start(ctx, StimulusOptions::default())?;
    common::select_window(&mut app, &stimulus.title)?;
    let (result, file, wall) = common::record_for(&mut app, 10.0)?;
    stimulus.stop();
    let video = common::judge_video(
        ctx,
        &file,
        &stimulus,
        &VideoExpectation {
            codec: "hevc",
            fps: 30.0,
            max_hold_s: 0.25,
            min_fresh_fraction: 0.5,
        },
    )?;
    product_ensure!(
        video.probe["format"]["format_name"]
            .as_str()
            .is_some_and(|name| name.contains("mp4")),
        "the recording is not an MP4 file"
    );
    product_ensure!(
        common::audio_streams(&video.probe) == 0,
        "an audio track was written although recording was disabled on every audio track"
    );
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("recordWallSeconds", wall);
    ctx.evidence.put("videoDurationSeconds", video.duration);
    Ok(())
}

fn pause_gap_is_clean(ids: &[u32], start: u32, end: u32) -> bool {
    start < end
        && ids.iter().any(|id| *id <= start)
        && ids.iter().any(|id| *id >= end)
        && ids.iter().all(|id| *id <= start || *id >= end)
}

fn latest_stimulus_id(stimulus: &Stimulus) -> Step<u32> {
    stimulus
        .frames()?
        .last()
        .map(|frame| frame.0)
        .ok_or_else(|| Stop::infra("the stimulus has no logged frames"))
}

fn pause_resume(ctx: &mut Context) -> Step {
    recording_workflow(ctx, false)
}

fn recording_edit_export(ctx: &mut Context) -> Step {
    recording_workflow(ctx, true)
}

fn recording_workflow(ctx: &mut Context, export: bool) -> Step {
    let mut app = ctx.launch(&[])?;
    if export && let Some(monitor) = common::target_monitor() {
        let environment = app.call("environment.snapshot", json!({}))?;
        let screens = environment["displays"]["screens"]
            .as_array()
            .ok_or_else(|| Stop::infra("the product reported no screen inventory"))?;
        let screen = screens
            .iter()
            .find(|screen| screen["device"] == monitor)
            .and_then(|screen| screen["name"].as_str())
            .ok_or_else(|| Stop::infra(format!("the product has no screen for {monitor}")))?;
        infra_ensure!(
            screens.iter().filter(|s| s["name"] == screen).count() == 1,
            "the requested display has an ambiguous Qt screen name: {screen}"
        );
        app.call("window.moveToScreen", json!({"screen": screen}))?;
        let window = app
            .client
            .poll("window.snapshot", json!({}), secs(10.0), |s| {
                s["screen"]["name"] == screen
            })?
            .ok_or_else(|| {
                Stop::fail("the application did not move to the requested test display")
            })?;
        ctx.evidence.put("testWindow", window);
    }
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.frameRate", json!(30)),
            ("video.cfr", json!(true)),
        ],
    )?;
    let mut stimulus = Stimulus::start(ctx, StimulusOptions::default())?;
    common::select_window(&mut app, &stimulus.title)?;
    common::disable_recording_audio(&mut app)?;
    common::start_recording(&mut app)?;
    std::thread::sleep(secs(3.0));
    common::pause(&mut app)?;
    let pause_start_id = latest_stimulus_id(&stimulus)?;
    std::thread::sleep(secs(3.0));
    let pause_end_id = latest_stimulus_id(&stimulus)?;
    common::resume(&mut app)?;
    std::thread::sleep(secs(3.0));
    let result = common::stop_recording(&mut app)?;
    let file = common::output_path(&result)?;
    stimulus.stop();

    let frames = crate::media::luma_frames(&file, 480)?;
    let timeline = crate::media::id_timeline(&frames);
    let ids: Vec<u32> = timeline.samples.iter().map(|(_, id)| *id).collect();
    product_ensure!(
        timeline.unique >= 90
            && timeline.corrupt == 0
            && timeline.absent == 0
            && timeline.reorders == 0,
        "the paused recording has too few distinct decoded stimulus frames, damaged frames or reordered ids"
    );
    product_ensure!(
        pause_end_id.saturating_sub(pause_start_id) >= 60,
        "the stimulus did not advance enough while the product was paused"
    );
    let guarded_start = pause_start_id.saturating_add(15);
    let guarded_end = pause_end_id.saturating_sub(15);
    product_ensure!(
        pause_gap_is_clean(&ids, guarded_start, guarded_end),
        "frames from the paused interval reached the output or one side of the pause is absent"
    );
    let duration =
        frames.last().map(|f| f.pts).unwrap_or(0.0) - frames.first().map(|f| f.pts).unwrap_or(0.0);
    product_ensure!(
        (4.5..=8.0).contains(&duration),
        "the file lasts {duration:.2} s although two 3 s recording slices were separated by a 3 s pause"
    );
    ctx.evidence.put("pauseStartStimulusId", pause_start_id);
    ctx.evidence.put("pauseEndStimulusId", pause_end_id);
    ctx.evidence.put("recordedDurationSeconds", duration);
    ctx.evidence
        .put("idTimeline", serde_json::to_value(&timeline)?);
    ctx.evidence.put("recordResult", result);
    ctx.keep(&file);
    if export {
        completed_recording_export(ctx, &mut app, &file, &timeline)?;
        let exit = app.close_gracefully(secs(60.0))?;
        ctx.evidence.put("cleanExitCode", exit.code);
    }
    Ok(())
}

fn completed_recording_export(
    ctx: &mut Context,
    app: &mut crate::context::App,
    source: &std::path::Path,
    expected: &crate::media::IdTimeline,
) -> Step {
    let request = |app: &mut crate::context::App, command, params| {
        app.client
            .request(command, params, secs(30.0))?
            .map_err(|refusal| Stop::fail(format!("{command} refused: {refusal}")))
    };
    let review = app
        .client
        .poll("review.snapshot", json!({}), secs(60.0), |s| {
            s["active"] == true && s["clipOpen"] == true
        })?;
    let review = match review {
        Some(review) => review,
        None => {
            let review = request(app, "review.snapshot", json!({}))?;
            let message = format!(
                "the completed recording did not open in Review: active={}, clipOpen={}, error={}, preview={}",
                review["active"], review["clipOpen"], review["error"], review["placeholderText"]
            );
            ctx.evidence.put("recordingReview", review);
            return Err(Stop::fail(message));
        }
    };
    product_ensure!(
        review["clipPath"]
            .as_str()
            .is_some_and(|p| std::path::Path::new(p) == source)
            && review["recordingIdentity"]
                .as_str()
                .is_some_and(|id| !id.is_empty()),
        "Recording Review lost the completed recording identity"
    );
    if review["playing"] == true {
        request(app, "review.playPause", json!({}))?;
        app.client
            .poll("review.snapshot", json!({}), secs(30.0), |s| {
                s["playing"] == false && s["transportPending"] == false
            })?
            .ok_or_else(|| Stop::fail("Review did not pause its initial playback"))?;
    }
    request(app, "review.playPause", json!({}))?;
    app.client
        .poll("review.snapshot", json!({}), secs(30.0), |s| {
            s["playing"] == true && s["transportPending"] == false
        })?
        .ok_or_else(|| Stop::fail("Review playback did not start"))?;
    request(app, "review.playPause", json!({}))?;
    app.client
        .poll("review.snapshot", json!({}), secs(30.0), |s| {
            s["playing"] == false && s["transportPending"] == false
        })?
        .ok_or_else(|| Stop::fail("Review playback did not pause"))?;
    let review_position = review["durationMs"]
        .as_f64()
        .filter(|d| d.is_finite() && *d > 0.0)
        .ok_or_else(|| Stop::fail("Review has no positive media duration"))?
        / 2.0;
    request(
        app,
        "review.seek",
        json!({"positionMs": review_position as i64}),
    )?;
    let sought_review = app
        .client
        .poll("review.snapshot", json!({}), secs(30.0), |s| {
            s["transportPending"] == false
                && s["positionMs"]
                    .as_f64()
                    .is_some_and(|p| (p - review_position).abs() <= 1.0)
        })?
        .ok_or_else(|| Stop::fail("Review did not seek within the completed recording"))?;
    product_ensure!(
        sought_review["recordingIdentity"] == review["recordingIdentity"]
            && sought_review["media"] == review["media"],
        "Review transport changed source metadata"
    );
    ctx.evidence.put("recordingReview", sought_review);
    request(app, "review.edit", json!({}))?;
    app.client
        .poll("review.snapshot", json!({}), secs(60.0), |s| {
            s["active"] == false && s["pendingRecordingIdentity"] == ""
        })?
        .ok_or_else(|| Stop::fail("Review did not complete its identity-based library handoff"))?;
    product_ensure!(
        request(app, "ui.getState", json!({}))?["page"] == "edit",
        "Review handoff did not navigate to Edit"
    );
    request(app, "edit.appendRecording", json!({"path": source}))?;
    let opened = app
        .client
        .poll("editor.snapshot", json!({}), secs(60.0), |s| {
            s["open"] == true && s["clipOpen"] == true
        })?
        .ok_or_else(|| Stop::fail("the completed recording did not open in Edit"))?;
    product_ensure!(
        opened["clipPath"]
            .as_str()
            .is_some_and(|p| std::path::Path::new(p) == source),
        "the completed recording handoff opened a different file"
    );
    let position = opened["durationMs"]
        .as_f64()
        .filter(|d| d.is_finite() && *d > 0.0)
        .ok_or_else(|| Stop::fail("Edit has no positive media duration"))?
        / 2.0;
    let sought = request(app, "edit.seek", json!({"positionMs": position as i64}))?;
    product_ensure!(
        sought["positionMs"]
            .as_f64()
            .is_some_and(|p| (p - position).abs() <= 1.0),
        "Edit did not seek within the completed recording"
    );
    ctx.evidence.put("recordingEditor", sought);
    request(
        app,
        "export.setOptions",
        json!({"span": "entire", "container": "mkv", "mode": "finishedStereo"}),
    )?;
    request(app, "export.start", json!({}))?;
    let review = app
        .client
        .poll("ui.getState", json!({}), secs(60.0), |s| {
            matches!(s["exportState"].as_str(), Some("review" | "failed"))
        })?
        .ok_or_else(|| Stop::fail("recording export did not reach review"))?;
    product_ensure!(
        review["exportState"] == "review",
        "recording export planning failed"
    );
    request(app, "export.confirm", json!({}))?;
    let finished = app
        .client
        .poll("ui.getState", json!({}), secs(180.0), |s| {
            matches!(s["exportState"].as_str(), Some("completed" | "failed"))
        })?
        .ok_or_else(|| Stop::fail("recording export did not finish"))?;
    product_ensure!(
        finished["exportState"] == "completed",
        "recording export failed"
    );
    let exported = request(app, "editor.snapshot", json!({}))?["export"].clone();
    let output = std::path::PathBuf::from(exported["outputPath"].as_str().unwrap_or_default());
    product_ensure!(
        output.parent() == Some(app.output.as_path()) && output.is_file() && output != source,
        "recording export did not create a distinct isolated output"
    );
    ctx.keep(&output);
    let probe = crate::media::probe(&output)?;
    let source_probe = crate::media::probe(source)?;
    let source_video = crate::media::streams(&source_probe, "video");
    let output_video = crate::media::streams(&probe, "video");
    product_ensure!(
        source_video.len() == 1
            && output_video.len() == 1
            && crate::media::streams(&source_probe, "audio").is_empty()
            && crate::media::streams(&probe, "audio").is_empty(),
        "the video-only recording and export must each contain one video stream and no audio"
    );
    product_ensure!(
        probe["format"]["format_name"]
            .as_str()
            .is_some_and(|name| name.split(',').any(|part| part == "matroska"))
            && output_video[0]["codec_name"] == "h264"
            && output_video[0]["width"] == source_video[0]["width"]
            && output_video[0]["height"] == source_video[0]["height"],
        "recording export changed the requested MKV/H.264 format or recording dimensions"
    );
    let frames = crate::media::luma_frames(&output, 480)?;
    let actual = crate::media::id_timeline(&frames);
    product_ensure!(
        actual.corrupt == 0 && actual.absent == 0 && actual.reorders == 0,
        "recording export contains damaged, absent or reordered frame identities"
    );
    product_ensure!(
        same_frame_timeline(&expected.samples, &actual.samples),
        "recording export changed frame identities, frame count or relative timestamps"
    );
    ctx.evidence.put("exportResult", exported);
    ctx.evidence.put("exportProbe", probe);
    ctx.evidence
        .put("exportTimeline", serde_json::to_value(actual)?);
    request(app, "export.reset", json!({}))?;
    request(app, "edit.close", json!({}))?;
    request(
        app,
        "edit.importMedia",
        json!({"path": output, "append": true}),
    )?;
    let reopened = app
        .client
        .poll("editor.snapshot", json!({}), secs(60.0), |s| {
            s["open"] == true && s["clipOpen"] == true
        })?
        .ok_or_else(|| Stop::fail("the finished export did not reopen in Edit"))?;
    product_ensure!(
        reopened["clips"].as_array().is_some_and(|clips| {
            clips.len() == 1
                && clips[0]["available"] == true
                && clips[0]["video"] == true
                && clips[0]["path"]
                    .as_str()
                    .is_some_and(|p| std::path::Path::new(p) == output)
        }),
        "reopening the export selected a different media file"
    );
    ctx.evidence.put("reopenedExport", reopened);
    Ok(())
}

fn same_frame_timeline(expected: &[(f64, u32)], actual: &[(f64, u32)]) -> bool {
    let (Some(first_expected), Some(first_actual)) = (expected.first(), actual.first()) else {
        return false;
    };
    expected.len() == actual.len()
        && expected.iter().zip(actual).all(|(a, b)| {
            a.1 == b.1 && ((a.0 - first_expected.0) - (b.0 - first_actual.0)).abs() <= 0.002
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn export_oracle_rejects_missing_changed_and_retimed_frames() {
        let source = [(1.0, 10), (1.033, 11), (1.067, 12)];
        assert!(same_frame_timeline(
            &source,
            &[(0.0, 10), (0.033, 11), (0.067, 12)]
        ));
        assert!(!same_frame_timeline(&source, &source[..2]));
        assert!(!same_frame_timeline(
            &source,
            &[(1.0, 10), (1.033, 99), (1.067, 12)]
        ));
        assert!(!same_frame_timeline(
            &source,
            &[(1.0, 10), (1.040, 11), (1.067, 12)]
        ));
        assert!(!same_frame_timeline(&[], &[]));
    }

    #[test]
    fn pause_oracle_requires_both_sides_and_no_middle_frames() {
        assert!(pause_gap_is_clean(&[10, 30, 200, 230], 40, 180));
        assert!(!pause_gap_is_clean(&[10, 30, 100, 200, 230], 40, 180));
        assert!(!pause_gap_is_clean(&[10, 30], 40, 180));
        assert!(!pause_gap_is_clean(&[200, 230], 40, 180));
    }
}
