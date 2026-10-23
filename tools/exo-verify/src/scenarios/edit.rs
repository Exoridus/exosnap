//! The editor, driven only through its semantic control commands.
//!
//! The product runs on Qt's offscreen platform: no window appears, no input is
//! synthesised and the desktop is never touched. Every step names the editor
//! intent a user would trigger, reads the product state that intent should
//! have produced, and the exported file is judged by ffprobe and the decoded
//! frame-id barcode rather than by the product's own report.
//!
//! Playback and the preview picture are not judged here. Without the
//! Direct3D 11 scene graph the preview cannot present, and that boundary is
//! the native acceptance run's.

use serde_json::{Value, json};
use std::path::{Path, PathBuf};

use super::common::{self, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::control::Refusal;
use crate::media;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "edit.timeline-semantic",
        revision: 1,
        title: "The editor imports, edits, undoes and exports through semantic control",
        class: ScenarioClass::Regression,
        contract: "imported media arrives as linked clips, a split and a ripple delete change the timeline exactly once each and report the revision they moved, undo and redo restore it, a crossfade and its junction settings round-trip, an overlap the model refuses is reported in the timeline's own words without a change, panels and options settle on what the page shows, a stale revision is refused before any edit, and the reviewed export writes the composed frames and tones into the isolated output folder",
        lane: Lane::Preflight,
        also: &[],
        tier: Tier::Recommended,
        requires: &[
            Capability::Ffmpeg,
            Capability::Ffprobe,
            Capability::D3d11,
            Capability::Nvenc,
        ],
        timeout: secs(300.0),
        run: timeline_semantic,
    }]
}

const FPS: u32 = 30;
const FIRST_A: u32 = 1000;
const FRAMES_A: u32 = 120;
const FIRST_B: u32 = 2000;
const FRAMES_B: u32 = 90;
const TONE_A: u32 = 440;
const TONE_B: u32 = 880;
/// Where the first source is split and its remainder rippled away.
const CUT_MS: i64 = 1000;
/// One frame at the sources' rate, the tolerance for any duration.
const FRAME_MS: f64 = 1000.0 / FPS as f64;

fn accepted(app: &mut App, command: &str, params: Value) -> Step<Value> {
    app.client
        .request(command, params, secs(30.0))?
        .map_err(|refusal| Stop::fail(format!("{command} refused: {refusal}")))
}

fn refused(app: &mut App, command: &str, params: Value) -> Step<Refusal> {
    match app.client.request(command, params, secs(30.0))? {
        Ok(result) => Err(Stop::fail(format!(
            "{command} was accepted where it must be refused: {result}"
        ))),
        Err(refusal) => Ok(refusal),
    }
}

fn state(app: &mut App) -> Step<Value> {
    Ok(app.call("ui.getState", json!({}))?)
}

fn editor(app: &mut App) -> Step<Value> {
    Ok(app.call("editor.snapshot", json!({}))?)
}

fn revision(app: &mut App) -> Step<u64> {
    state(app)?["editor"]["workspaceRevision"]
        .as_u64()
        .ok_or_else(|| Stop::fail("ui.getState carries no editor workspace revision"))
}

/// Waits until an import has landed: nothing pending and `clips` clips on
/// the timeline. A workspace error instead is the product refusing the media.
fn wait_for_clips(app: &mut App, clips: u64) -> Step<Value> {
    let settled = app
        .client
        .poll("ui.getState", json!({}), secs(30.0), |s| {
            let e = &s["editor"];
            e["importPending"] == false
                && (e["clipCount"].as_u64() == Some(clips) || !e["workspaceError"].is_null())
        })?
        .ok_or_else(|| {
            Stop::fail(format!(
                "the timeline did not reach {clips} clips within 30 s"
            ))
        })?;
    product_ensure!(
        settled["editor"]["workspaceError"].is_null(),
        "the import was refused: {}",
        settled["editor"]["workspaceError"]
    );
    Ok(settled)
}

fn video_clips(snapshot: &Value) -> Vec<Value> {
    let mut clips: Vec<Value> = snapshot["clips"]
        .as_array()
        .map(|clips| {
            clips
                .iter()
                .filter(|c| c["video"] == true)
                .cloned()
                .collect()
        })
        .unwrap_or_default();
    clips.sort_by_key(|c| c["startMs"].as_i64().unwrap_or(i64::MAX));
    clips
}

fn near(actual: &Value, expected_ms: f64) -> bool {
    actual
        .as_f64()
        .is_some_and(|ms| (ms - expected_ms).abs() <= FRAME_MS)
}

fn edit_report(result: &Value) -> Step<&Value> {
    let edit = &result["edit"];
    product_ensure!(
        edit.is_object(),
        "a timeline edit answered without its edit report: {result}"
    );
    Ok(edit)
}

/// An accepted edit moved the model revision by exactly one.
fn judge_one_step(result: &Value, what: &str) -> Step {
    let edit = edit_report(result)?;
    let before = edit["revisionBefore"].as_u64().unwrap_or(0);
    let after = edit["revisionAfter"].as_u64().unwrap_or(0);
    product_ensure!(
        edit["changed"] == true && after == before + 1,
        "{what} did not move the timeline revision by one: {edit}"
    );
    Ok(())
}

fn import(app: &mut App, path: &Path, clips: u64) -> Step<Value> {
    let answer = accepted(
        app,
        "edit.importMedia",
        json!({ "path": path.display().to_string(), "append": true }),
    )?;
    product_ensure!(
        answer.get("open").is_some(),
        "edit.importMedia did not answer with the editor snapshot"
    );
    wait_for_clips(app, clips)
}

fn timeline_semantic(ctx: &mut Context) -> Step {
    let sources = ctx.scenario_dir.join("sources");
    std::fs::create_dir_all(&sources)?;
    let source_a = sources.join("source-a.mkv");
    let source_b = sources.join("source-b.mkv");
    media::encode_edit_source(FIRST_A, FRAMES_A, FPS, TONE_A, &source_a)?;
    media::encode_edit_source(FIRST_B, FRAMES_B, FPS, TONE_B, &source_b)?;

    let mut app = ctx.launch_offscreen(&[])?;
    // The render path encodes with the configured codec; H.264 is the one
    // every supported encoder offers.
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
        ],
    )?;
    accepted(&mut app, "ui.navigate", json!({ "page": "edit" }))?;
    let empty = state(&mut app)?;
    infra_ensure!(
        empty["editor"]["clipCount"] == 0,
        "the isolated profile opened Edit with content: {}",
        empty["editor"]
    );
    let offered: Vec<&str> = empty["availableActions"]
        .as_array()
        .map(|a| a.iter().filter_map(Value::as_str).collect())
        .unwrap_or_default();
    product_ensure!(
        offered.contains(&"edit.importMedia") && !offered.contains(&"edit.split"),
        "an empty workspace offers the wrong timeline actions: {offered:?}"
    );

    // --- Import: one linked group per source, appended in order --------------
    import(&mut app, &source_a, 2)?;
    let after_b = import(&mut app, &source_b, 4)?;
    ctx.evidence.put("importedState", after_b["editor"].clone());
    let imported = editor(&mut app)?;
    let video = video_clips(&imported);
    product_ensure!(
        video.len() == 2
            && near(&video[0]["durationMs"], 4000.0)
            && near(&video[1]["startMs"], 4000.0)
            && near(&video[1]["durationMs"], 3000.0)
            && near(&imported["durationMs"], 7000.0),
        "the two sources did not land back to back: {}",
        json!(video)
    );
    for clip in &video {
        let group = &clip["group"];
        let linked = imported["clips"]
            .as_array()
            .map(|all| all.iter().filter(|c| &c["group"] == group).count())
            .unwrap_or(0);
        product_ensure!(
            linked == 2 && clip["available"] == true,
            "a source did not arrive as an available linked video/audio pair: {clip}"
        );
    }

    // --- A stale revision is refused before anything runs ---------------------
    let current = revision(&mut app)?;
    let stale = refused(
        &mut app,
        "edit.split",
        json!({ "expectedRevision": current.saturating_sub(1) }),
    )?;
    product_ensure!(
        stale.code == "invalid_state"
            && stale.requires["workspaceRevision"].as_u64() == Some(current.saturating_sub(1))
            && stale.actual["workspaceRevision"].as_u64() == Some(current),
        "a stale revision was not refused with both revisions: {stale} {} {}",
        stale.requires,
        stale.actual
    );
    product_ensure!(
        revision(&mut app)? == current,
        "a refused stale edit still changed the timeline"
    );

    // --- Split at the playhead and ripple the remainder away --------------------
    accepted(&mut app, "edit.seek", json!({ "positionMs": CUT_MS }))?;
    accepted(
        &mut app,
        "edit.selectClip",
        json!({ "clipId": video[0]["id"] }),
    )?;
    let split = accepted(
        &mut app,
        "edit.split",
        json!({ "expectedRevision": current }),
    )?;
    judge_one_step(&split, "split")?;
    let pieces = video_clips(&split);
    product_ensure!(
        pieces.len() == 3 && near(&pieces[1]["startMs"], CUT_MS as f64),
        "the split did not cut the first source at the playhead: {}",
        json!(pieces)
    );
    accepted(
        &mut app,
        "edit.selectClip",
        json!({ "clipId": pieces[1]["id"] }),
    )?;
    let rippled = accepted(&mut app, "edit.delete", json!({ "ripple": true }))?;
    judge_one_step(&rippled, "ripple delete")?;
    let kept = video_clips(&rippled);
    product_ensure!(
        kept.len() == 2
            && near(&kept[1]["startMs"], CUT_MS as f64)
            && near(&rippled["durationMs"], CUT_MS as f64 + 3000.0),
        "ripple delete did not close the removed time: {}",
        json!(kept)
    );

    // --- History -----------------------------------------------------------------
    let undone = accepted(&mut app, "edit.undo", json!({}))?;
    judge_one_step(&undone, "undo")?;
    product_ensure!(
        near(&undone["durationMs"], 7000.0) && video_clips(&undone).len() == 3,
        "undo did not restore the removed clip"
    );
    let redone = accepted(&mut app, "edit.redo", json!({}))?;
    judge_one_step(&redone, "redo")?;
    product_ensure!(
        near(&redone["durationMs"], 4000.0) && video_clips(&redone).len() == 2,
        "redo did not reapply the ripple delete"
    );

    // A gap delete commits the close-gap preview before answering.
    accepted(
        &mut app,
        "edit.moveClip",
        json!({ "clipId": kept[1]["id"], "atMs": 1500, "snap": false }),
    )?;
    let track_index = kept[1]["trackIndex"]
        .as_u64()
        .ok_or_else(|| Stop::fail("the video clip has no track index"))?
        as usize;
    let track_id = &imported["tracks"][track_index]["id"];
    accepted(
        &mut app,
        "edit.selectGap",
        json!({ "trackId": track_id, "atMs": 1250 }),
    )?;
    let closed_gap = accepted(&mut app, "edit.delete", json!({ "ripple": true }))?;
    judge_one_step(&closed_gap, "ripple gap delete")?;
    let gap_video = video_clips(&closed_gap);
    product_ensure!(
        gap_video.len() == 2
            && near(&closed_gap["durationMs"], 4000.0)
            && near(&gap_video[1]["startMs"], 1000.0),
        "ripple gap delete left the gap on the timeline"
    );
    accepted(&mut app, "edit.undo", json!({}))?;
    accepted(&mut app, "edit.undo", json!({}))?;

    // --- A crossfade, its junction settings, and its removal by undo ---------------
    let crossfaded = accepted(
        &mut app,
        "edit.applyCrossfade",
        json!({ "clipId": kept[0]["id"], "durationMs": 500 }),
    )?;
    judge_one_step(&crossfaded, "crossfade")?;
    let transitions = crossfaded["transitions"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    let audio_junction = transitions.iter().find(|t| t["video"] == false).cloned();
    product_ensure!(
        transitions
            .iter()
            .any(|t| t["video"] == true && t["kind"] == "crossfade")
            && audio_junction.is_some()
            && near(&crossfaded["durationMs"], 3500.0),
        "the crossfade did not overlap the linked cut by 500 ms: {}",
        json!(transitions)
    );
    let audio_junction = audio_junction.unwrap_or_default();
    accepted(
        &mut app,
        "edit.selectTransition",
        json!({ "clipId": audio_junction["outgoing"] }),
    )?;
    let law = accepted(
        &mut app,
        "edit.setJunctionAudioLaw",
        json!({ "law": "equalPower" }),
    )?;
    product_ensure!(
        law["selection"]["transition"]["audioLaw"] == "equalPower",
        "the audio junction did not take the equal-power law: {}",
        law["selection"]["transition"]
    );
    accepted(&mut app, "edit.undo", json!({}))?;
    let no_crossfade = accepted(&mut app, "edit.undo", json!({}))?;
    product_ensure!(
        no_crossfade["transitions"]
            .as_array()
            .is_some_and(Vec::is_empty)
            && near(&no_crossfade["durationMs"], 4000.0),
        "two undos did not remove the junction setting and the crossfade"
    );

    // --- A refusal in the timeline's own words, and nothing changed -------------
    let before_refusal = revision(&mut app)?;
    let containment = refused(
        &mut app,
        "edit.moveClip",
        json!({ "clipId": kept[0]["id"], "atMs": CUT_MS + 500, "snap": false }),
    )?;
    product_ensure!(
        containment.code == "blocked"
            && !containment.message.is_empty()
            && containment.actual["edit"] == "refused",
        "an overlap the model rejects was not reported as its refusal: {containment}"
    );
    product_ensure!(
        revision(&mut app)? == before_refusal,
        "a refused move changed the timeline"
    );
    let header = state(&mut app)?["editor"]["workspaceError"].clone();
    product_ensure!(
        header.as_str() == Some(containment.message.as_str()),
        "the refusal and the timeline header disagree: {header} vs {}",
        containment.message
    );

    // --- Options and panels settle on what the page reports ----------------------
    let ripple = accepted(
        &mut app,
        "edit.setOption",
        json!({ "option": "ripple", "enabled": true }),
    )?;
    product_ensure!(
        ripple["options"]["ripple"] == true,
        "Ripple did not turn on"
    );
    accepted(
        &mut app,
        "edit.setOption",
        json!({ "option": "ripple", "enabled": false }),
    )?;
    let details = accepted(
        &mut app,
        "edit.setPanel",
        json!({ "panel": "details", "open": true }),
    )?;
    product_ensure!(
        details["panels"]["details"] == "open",
        "Details did not open: {}",
        details["panels"]
    );
    let tab = accepted(
        &mut app,
        "edit.setSourceTab",
        json!({ "tab": "transitions" }),
    )?;
    product_ensure!(
        tab["panels"]["sourceTab"] == "transitions",
        "the Transitions tab did not show"
    );
    let media_panel = accepted(
        &mut app,
        "edit.setPanel",
        json!({ "panel": "media", "open": true }),
    )?;
    ctx.evidence.put("panels", media_panel["panels"].clone());
    accepted(
        &mut app,
        "edit.setPanel",
        json!({ "panel": "media", "open": false }),
    )?;

    // --- The reviewed export ------------------------------------------------------
    accepted(
        &mut app,
        "export.setOptions",
        json!({ "span": "entire", "container": "mkv", "mode": "finishedStereo" }),
    )?;
    accepted(&mut app, "export.start", json!({}))?;
    let reviewed = app
        .client
        .poll("ui.getState", json!({}), secs(60.0), |s| {
            matches!(s["exportState"].as_str(), Some("review" | "failed"))
        })?
        .ok_or_else(|| Stop::fail("the export plan did not reach its review within 60 s"))?;
    let plan = editor(&mut app)?["export"].clone();
    ctx.evidence.put("exportReview", plan.clone());
    product_ensure!(
        reviewed["exportState"] == "review",
        "the export was refused before review: {}",
        plan["errorText"]
    );
    product_ensure!(
        plan["review"]["path"] == "Render" && plan["review"]["audioSummary"] == "Stereo",
        "the review does not describe a rendered stereo result: {}",
        plan["review"]
    );
    accepted(&mut app, "export.confirm", json!({}))?;
    let finished = app
        .client
        .poll("ui.getState", json!({}), secs(180.0), |s| {
            matches!(s["exportState"].as_str(), Some("completed" | "failed"))
        })?
        .ok_or_else(|| Stop::fail("the export did not finish within 180 s"))?;
    let result = editor(&mut app)?["export"].clone();
    product_ensure!(
        finished["exportState"] == "completed",
        "the export failed: {}",
        result["errorText"]
    );
    let output = PathBuf::from(result["outputPath"].as_str().unwrap_or_default());
    product_ensure!(
        output.parent() == Some(app.output.as_path()) && output.is_file(),
        "the export did not land in the isolated output folder: {}",
        output.display()
    );
    ctx.keep(&output);
    judge_export(ctx, &output)?;

    let reset = accepted(&mut app, "export.reset", json!({}))?;
    product_ensure!(
        reset["export"]["state"] == "idle",
        "the export options did not return"
    );
    let closed = accepted(&mut app, "edit.close", json!({}))?;
    product_ensure!(
        closed["open"] == false && state(&mut app)?["editSession"] == "closed",
        "closing the editor left a session"
    );
    let exit = app.close_gracefully(secs(60.0))?;
    ctx.evidence.put("exitCode", exit.code);
    Ok(())
}

/// The composed timeline, judged from the file alone: the first second is the
/// head of source A (ids from 1000, 440 Hz), the rest is source B from its
/// first frame (ids from 2000, 880 Hz).
fn judge_export(ctx: &mut Context, output: &Path) -> Step {
    let probe = media::probe(output)?;
    let duration = media::f64_field(&probe["format"], "duration").unwrap_or(0.0);
    product_ensure!(
        (duration - 4.0).abs() <= 2.0 / FPS as f64,
        "the export lasts {duration} s, the timeline 4 s"
    );
    let audio = media::streams(&probe, "audio");
    product_ensure!(
        media::streams(&probe, "video").len() == 1
            && audio.len() == 1
            && audio[0]["channels"] == 2
            && audio[0]["sample_rate"] == "48000",
        "the export is not one video stream and one 48 kHz stereo mix"
    );

    let frames = media::luma_frames(output, 320)?;
    let timeline = media::id_timeline(&frames);
    let cut_s = CUT_MS as f64 / 1000.0;
    let mut wrong = Vec::new();
    for &(pts, id) in &timeline.samples {
        let expected = if pts + 0.5 / f64::from(FPS) < cut_s {
            FIRST_A + (pts * FPS as f64).round() as u32
        } else {
            FIRST_B + ((pts - cut_s) * FPS as f64).round() as u32
        };
        if id != expected {
            wrong.push(json!({ "pts": pts, "id": id, "expected": expected }));
        }
    }
    ctx.evidence.put("exportIds", json!(timeline));
    ctx.evidence.put("exportWrongFrames", json!(wrong));
    product_ensure!(
        timeline.decoded * 100 >= timeline.frames * 95 && timeline.frames >= 110,
        "only {} of {} exported frames carry a readable id",
        timeline.decoded,
        timeline.frames
    );
    product_ensure!(
        wrong.is_empty(),
        "{} exported frames show the wrong source frame, first {}",
        wrong.len(),
        wrong[0]
    );

    let samples = media::audio_samples(output, 0, 48000)?;
    product_ensure!(
        samples.len() >= (3.8 * 48000.0) as usize,
        "the exported audio ends before the last tone window: {} samples",
        samples.len()
    );
    let head = dominant_hz(&samples[..(0.9 * 48000.0) as usize], 48000);
    let tail = dominant_hz(
        &samples[(1.2 * 48000.0) as usize..(3.8 * 48000.0) as usize],
        48000,
    );
    ctx.evidence
        .put("exportToneHz", json!({ "head": head, "tail": tail }));
    product_ensure!(
        (head - TONE_A as f64).abs() < 15.0 && (tail - TONE_B as f64).abs() < 15.0,
        "the mixed audio does not follow the cut: {head:.0} Hz then {tail:.0} Hz"
    );
    Ok(())
}

/// The frequency of a pure tone from its zero crossings.
fn dominant_hz(samples: &[f32], rate: u32) -> f64 {
    if samples.len() < 2 {
        return 0.0;
    }
    let crossings = samples
        .windows(2)
        .filter(|pair| (pair[0] < 0.0) != (pair[1] < 0.0))
        .count();
    crossings as f64 / 2.0 / (samples.len() as f64 / f64::from(rate))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_pure_tone_reads_back_at_its_frequency() {
        let rate = 48000;
        let tone: Vec<f32> = (0..rate)
            .map(|n| (2.0 * std::f32::consts::PI * 440.0 * n as f32 / rate as f32 + 0.1).sin())
            .collect();
        assert!((dominant_hz(&tone, rate) - 440.0).abs() < 2.0);
        assert_eq!(dominant_hz(&[], rate), 0.0);
    }

    #[test]
    fn a_duration_is_near_within_one_frame() {
        assert!(near(&json!(4000.0 + FRAME_MS - 1.0), 4000.0));
        assert!(!near(&json!(4000.0 + FRAME_MS + 1.0), 4000.0));
        assert!(!near(&Value::Null, 4000.0));
    }

    #[test]
    fn an_edit_must_move_the_revision_by_exactly_one() {
        let one = json!({"edit": {"changed": true, "revisionBefore": 4, "revisionAfter": 5}});
        judge_one_step(&one, "split").unwrap();
        for bad in [
            json!({"edit": {"changed": false, "revisionBefore": 4, "revisionAfter": 4}}),
            json!({"edit": {"changed": true, "revisionBefore": 4, "revisionAfter": 6}}),
            json!({}),
        ] {
            assert!(matches!(judge_one_step(&bad, "split"), Err(Stop::Fail(_))));
        }
    }

    #[test]
    fn video_clips_are_ordered_by_start_and_exclude_audio() {
        let snapshot = json!({"clips": [
            {"id": 2, "video": true, "startMs": 4000},
            {"id": 3, "video": false, "startMs": 0},
            {"id": 1, "video": true, "startMs": 0},
        ]});
        let ids: Vec<_> = video_clips(&snapshot)
            .iter()
            .map(|c| c["id"].clone())
            .collect();
        assert_eq!(ids, vec![json!(1), json!(2)]);
    }
}
