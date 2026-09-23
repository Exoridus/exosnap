//! Recording scenarios judged from the stimulus log and decoded output.

use serde_json::json;

use super::common::{self, Stimulus, StimulusOptions, VideoExpectation, secs};
use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;
use crate::product_ensure;
use crate::scenario::{Lane, Scenario, Step, Stop};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "record.ddx-h264-mkv",
            revision: 1,
            title: "A 60 fps display recording decodes with aligned system audio",
            claim: "DXGI display capture produces H.264 in MKV with a changing stimulus, ordered frames, decoded system audio and bounded A/V offset",
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
            claim: "WGC captures the changing window stimulus in a decodable HEVC MP4 with ordered frames and bounded holds",
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
            id: "record.pause-resume",
            revision: 1,
            title: "Pause removes time and frames from a finished recording",
            claim: "pausing a WGC recording suppresses the changing stimulus during the pause, resumes afterward and excludes paused wall time from the decoded file",
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
            ("audio.systemEnabled", json!(false)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
        ],
    )?;
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
        "an audio track was written although all audio sources were disabled"
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
        ],
    )?;
    let mut stimulus = Stimulus::start(ctx, StimulusOptions::default())?;
    common::select_window(&mut app, &stimulus.title)?;
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
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pause_oracle_requires_both_sides_and_no_middle_frames() {
        assert!(pause_gap_is_clean(&[10, 30, 200, 230], 40, 180));
        assert!(!pause_gap_is_clean(&[10, 30, 100, 200, 230], 40, 180));
        assert!(!pause_gap_is_clean(&[10, 30], 40, 180));
        assert!(!pause_gap_is_clean(&[200, 230], 40, 180));
    }
}
