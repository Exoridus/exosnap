//! Clip admission and the retained live recording oracle.

use serde_json::{Value, json};
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Instant;

use super::common::{self, Stimulus, StimulusOptions, VideoExpectation, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::control::Refusal;
use crate::media;
use crate::plan::Tier;
use crate::product_ensure;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "clip.creation-unavailable",
        revision: 1,
        title: "The product rejects Recording Clip creation without jobs or media",
        class: ScenarioClass::Contract,
        contract: "the production control endpoint reports Recording Clips unavailable, refuses requests and stale confirmations with the capability reason, and leaves no jobs, output or staging files through shutdown",
        lane: Lane::CiCore,
        also: &[Lane::Gpu, Lane::Preflight],
        tier: Tier::Required,
        requires: &[Capability::InteractiveDesktop],
        timeout: secs(120.0),
        run: creation_unavailable,
    }]
}

fn judge_unavailable(snapshot: &Value, output: &std::path::Path) -> Step {
    product_ensure!(
        snapshot["productAvailable"] == false
            && snapshot["canRequest"] == false
            && snapshot["jobs"].as_array().is_some_and(Vec::is_empty),
        "Recording Clip creation is exposed or admitted jobs: {snapshot}"
    );
    product_ensure!(
        std::fs::read_dir(output)?.next().is_none(),
        "a rejected clip request created output or staging entries in {}",
        output.display()
    );
    Ok(())
}

fn judge_refusal(response: Result<Value, Refusal>) -> Step {
    let refusal = response
        .err()
        .ok_or_else(|| Stop::fail("clip creation was accepted"))?;
    product_ensure!(
        refusal.code == "unavailable"
            && refusal.requires == json!({"recordingClipsAvailable": true})
            && refusal.actual == json!({"recordingClipsAvailable": false})
            && !refusal.message.trim().is_empty(),
        "clip creation did not report the unavailable product capability: {refusal}"
    );
    Ok(())
}

fn creation_unavailable(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let before = app.call("clip.snapshot", json!({}))?;
    judge_unavailable(&before, &app.output)?;
    ctx.evidence.put("clipBefore", before);
    for (command, params) in [
        ("clip.request", json!({"durationSeconds": "30"})),
        ("clip.request", json!({"durationSeconds": "1800"})),
        (
            "clip.confirm",
            json!({
                "jobId": "11111111-1111-4111-8111-111111111111",
                "generation": "1"
            }),
        ),
    ] {
        judge_refusal(app.client.request(command, params, secs(30.0))?)?;
        judge_unavailable(&app.call("clip.snapshot", json!({}))?, &app.output)?;
    }
    let after = app.call("clip.snapshot", json!({}))?;
    ctx.evidence.put("clipAfter", after.clone());
    ctx.evidence
        .put("controlTranscript", json!(app.client.transcript));
    let output = app.output.clone();
    app.close_gracefully(secs(20.0))?;
    judge_unavailable(&after, &output)?;
    Ok(())
}

#[allow(
    dead_code,
    reason = "Retained for source-level Clip reactivation after qualification"
)]
fn recording_live_scenario() -> Scenario {
    Scenario {
        id: "clip.recording-live",
        revision: 3,
        title: "A live recording saves an offered last-30-seconds clip while capture continues",
        class: ScenarioClass::Hardware,
        contract: "after 35 seconds of recording, the production clip adapter plans the offered last-30-seconds request, confirms its available range, and creates a decodable H.264/Opus MKV with relative timestamps and preserved audio identity while primary capture continues; the completed clip matches the reviewed range and opens and seeks in Edit",
        lane: Lane::Gpu,
        also: &[Lane::Preflight],
        tier: Tier::Required,
        requires: &[
            Capability::InteractiveDesktop,
            Capability::Wgc,
            Capability::Nvenc,
            Capability::AudioRender,
            Capability::AudioCapture,
            Capability::Ffprobe,
            Capability::Ffmpeg,
        ],
        timeout: secs(300.0),
        run: recording_live,
    }
}

fn accepted(app: &mut App, command: &str, params: Value) -> Step<Value> {
    app.client
        .request(command, params, secs(30.0))?
        .map_err(|refusal| Stop::fail(format!("{command} refused: {refusal}")))
}

fn require_recording(app: &mut App) -> Step {
    let state = app.call("record.snapshot", json!({}))?;
    product_ensure!(
        state["recording"] == true && state["paused"] == false,
        "the primary recording stopped or paused during clip creation: {state}"
    );
    Ok(())
}

fn manifest(probe: &Value) -> Step<Value> {
    let text = probe["format"]["tags"]
        .as_object()
        .and_then(|tags| {
            tags.iter()
                .find(|(key, _)| key.eq_ignore_ascii_case("EXOSNAP_AUDIO_MANIFEST"))
        })
        .and_then(|(_, value)| value.as_str())
        .ok_or_else(|| Stop::fail("the Matroska file has no audio manifest"))?;
    serde_json::from_str(text).map_err(|error| {
        Stop::fail(format!(
            "the written audio manifest is invalid JSON: {error}"
        ))
    })
}

fn range_seconds(job: &Value, key: &str) -> Step<f64> {
    let text = job[key]
        .as_str()
        .ok_or_else(|| Stop::infra(format!("clip job has no {key} time")))?;
    let values: Vec<u64> = text
        .split(':')
        .map(str::parse)
        .collect::<Result<_, _>>()
        .map_err(|error| Stop::infra(format!("clip {key} time is invalid: {error}")))?;
    if values.len() != 3 || values[1] >= 60 || values[2] >= 60 {
        return Err(Stop::infra(format!("clip {key} time is invalid: {text}")));
    }
    Ok((values[0] * 3600 + values[1] * 60 + values[2]) as f64)
}

fn judge_clip_marker(primary: &Value, clip: &Value, begin_ns: u64) -> Step {
    let primary_markers = primary["markers"]
        .as_array()
        .ok_or_else(|| Stop::fail("primary marker sidecar has no markers"))?;
    let clip_markers = clip["markers"]
        .as_array()
        .ok_or_else(|| Stop::fail("clip marker sidecar has no markers"))?;
    product_ensure!(
        primary_markers.len() == 1 && clip_markers.len() == 1,
        "the recording or clip lost its authored marker"
    );
    let primary_time = primary_markers[0]["timeMs"]
        .as_u64()
        .ok_or_else(|| Stop::fail("primary marker has no timestamp"))?;
    let clip_time = clip_markers[0]["timeMs"]
        .as_u64()
        .ok_or_else(|| Stop::fail("clip marker has no timestamp"))?;
    product_ensure!(
        primary_time.checked_sub(begin_ns.div_ceil(1_000_000)) == Some(clip_time)
            && primary_markers[0]["label"] == clip_markers[0]["label"]
            && primary_markers[0]["type"] == clip_markers[0]["type"],
        "clip marker timestamp or identity changed"
    );
    Ok(())
}

fn recording_live(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("video.container", json!("MKV")),
            ("video.videoCodec", json!("H.264")),
            ("video.audioCodec", json!("Opus")),
            ("video.frameRate", json!(30)),
            ("video.cfr", json!(true)),
        ],
    )?;
    let routing = app.call("audio.routing.snapshot", json!({}))?;
    product_ensure!(
        routing["tracks"].as_array().is_some_and(|tracks| tracks
            .iter()
            .filter(|track| track["recordEnabled"] == true)
            .count()
            == 1)
            && routing["sources"].as_array().is_some_and(|sources| {
                let participating: Vec<_> = sources
                    .iter()
                    .filter(|source| source["participation"] == true)
                    .collect();
                participating.len() == 1
                    && participating[0]["bindingKind"] == 0
                    && participating[0]["followDefault"] == true
                    && participating[0]["muted"] == false
            }),
        "the isolated recording does not have its default system-audio track: {routing}"
    );
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            marker_interval: Some(1.0),
            ..Default::default()
        },
    )?;
    if !stimulus.audio_available()? {
        return Err(Stop::unavailable(
            "the stimulus could not schedule WASAPI beeps",
        ));
    }
    common::select_window(&mut app, &stimulus.title)?;
    let started = common::start_recording(&mut app)?;
    std::thread::sleep(secs(3.0));
    accepted(&mut app, "ui.navigate", json!({ "page": "diagnostics" }))?;
    let state = app.call("ui.getState", json!({}))?;
    product_ensure!(
        state["page"] == "diagnostics",
        "Diagnostics did not become the active page"
    );
    std::thread::sleep(secs(3.0));
    let diagnostics = app.call("diagnostics.snapshot", json!({}))?;
    let pipeline = app.call("pipeline.snapshot", json!({}))?;
    product_ensure!(
        diagnostics["available"] == true
            && diagnostics["dataReady"] == true
            && pipeline["valid"] == true,
        "Diagnostics did not expose the running native pipeline"
    );
    require_recording(&mut app)?;
    ctx.evidence.put("runningDiagnostics", diagnostics);
    ctx.evidence.put("runningPipeline", pipeline);
    accepted(&mut app, "ui.navigate", json!({ "page": "record" }))?;
    product_ensure!(
        app.call("ui.getState", json!({}))?["page"] == "record",
        "Record did not become the active page"
    );
    if started.elapsed() < secs(20.0) {
        std::thread::sleep(secs(20.0) - started.elapsed());
    }
    accepted(&mut app, "record.addMarker", json!({}))?;
    if started.elapsed() < secs(35.0) {
        std::thread::sleep(secs(35.0) - started.elapsed());
    }
    require_recording(&mut app)?;
    let before = app.call("clip.snapshot", json!({}))?;
    product_ensure!(
        before["canRequest"] == true,
        "live clipping is unavailable: {before}"
    );
    let previous_ids: Vec<_> = before["jobs"]
        .as_array()
        .ok_or_else(|| Stop::infra("clip.snapshot has no jobs array"))?
        .iter()
        .filter_map(|job| job["jobId"].as_str().map(str::to_owned))
        .collect();
    let mut snapshot = accepted(&mut app, "clip.request", json!({ "durationSeconds": "30" }))?;
    let deadline = Instant::now() + secs(120.0);
    let mut selected_id: Option<String> = None;
    let mut confirmed_generation: Option<String> = None;
    let mut reviewed_range: Option<(Value, Value)> = None;
    let completed = loop {
        require_recording(&mut app)?;
        let jobs = snapshot["jobs"]
            .as_array()
            .ok_or_else(|| Stop::infra("clip snapshot has no jobs array"))?;
        let new_jobs: Vec<_> = jobs
            .iter()
            .filter(|job| {
                job["jobId"]
                    .as_str()
                    .is_some_and(|id| !previous_ids.iter().any(|old| old == id))
            })
            .collect();
        product_ensure!(
            new_jobs.len() <= 1,
            "one clip request created multiple jobs"
        );
        if let Some(job) = new_jobs.first() {
            product_ensure!(
                job["requestedDurationSeconds"] == "30",
                "the clip job changed the requested duration"
            );
            let id = job["jobId"].as_str().unwrap().to_owned();
            if let Some(selected) = &selected_id {
                product_ensure!(selected == &id, "the requested clip job changed identity");
            } else {
                selected_id = Some(id.clone());
            }
            match job["phase"].as_u64() {
                Some(8) => {
                    product_ensure!(
                        job["canOpen"] == true && job["hasActual"] == true,
                        "completed clip has no published asset: {job}"
                    );
                    break (*job).clone();
                }
                Some(10 | 11) => {
                    return Err(Stop::fail(format!("the clip did not complete: {job}")));
                }
                Some(1) if job["canConfirm"] == true => {
                    let generation = job["generation"]
                        .as_str()
                        .ok_or_else(|| Stop::infra("clip review has no generation string"))?
                        .to_owned();
                    if confirmed_generation.as_deref() != Some(generation.as_str()) {
                        reviewed_range =
                            Some((job["actualBegin"].clone(), job["actualEnd"].clone()));
                        ctx.evidence.put("clipReview", (*job).clone());
                        snapshot = accepted(
                            &mut app,
                            "clip.confirm",
                            json!({ "jobId": id, "generation": generation }),
                        )?;
                        confirmed_generation = Some(generation);
                    }
                }
                Some(_) => {}
                None => return Err(Stop::infra("clip job has no numeric phase")),
            }
        }
        product_ensure!(
            Instant::now() < deadline,
            "the live clip did not complete before the deadline: {snapshot}"
        );
        std::thread::sleep(secs(0.1));
        snapshot = app.call("clip.snapshot", json!({}))?;
    };
    if let Some((begin, end)) = reviewed_range {
        product_ensure!(
            completed["actualBegin"] == begin && completed["actualEnd"] == end,
            "the completed clip changed its confirmed range"
        );
    }
    let requested_begin = range_seconds(&completed, "requestedBegin")?;
    let requested_end = range_seconds(&completed, "requestedEnd")?;
    let actual_begin = range_seconds(&completed, "actualBegin")?;
    let actual_end = range_seconds(&completed, "actualEnd")?;
    product_ensure!(
        requested_end - requested_begin == 30.0
            && actual_begin < actual_end
            && actual_end <= requested_end,
        "the clip changed its requested window or reported an invalid available range"
    );
    ctx.evidence.put("completedClip", completed.clone());
    std::thread::sleep(secs(2.0));
    require_recording(&mut app)?;
    let primary_result = common::stop_recording(&mut app)?;
    let primary_path = common::output_path(&primary_result)?;
    let clip_path = PathBuf::from(
        completed["assetPath"]
            .as_str()
            .ok_or_else(|| Stop::infra("completed clip snapshot has no assetPath"))?,
    );
    product_ensure!(
        clip_path.is_file() && clip_path != primary_path,
        "the completed clip has no distinct existing file"
    );
    let primary_markers: Value =
        serde_json::from_slice(&std::fs::read(primary_path.with_extension("markers.json"))?)?;
    let clip_markers: Value =
        serde_json::from_slice(&std::fs::read(clip_path.with_extension("markers.json"))?)?;
    let mut identity_path = clip_path.as_os_str().to_owned();
    identity_path.push(".exosnap.json");
    let clip_identity: Value =
        serde_json::from_slice(&std::fs::read(PathBuf::from(identity_path))?)?;
    let begin_ns = clip_identity["source_ranges"][0]["begin_r_ns"]
        .as_str()
        .ok_or_else(|| Stop::fail("clip identity has no source origin"))?
        .parse::<u64>()
        .map_err(|error| Stop::fail(format!("invalid clip origin: {error}")))?;
    judge_clip_marker(&primary_markers, &clip_markers, begin_ns)?;
    product_ensure!(
        clip_identity["marker_refs"]
            .as_array()
            .is_some_and(|refs| refs.len() == 1
                && refs[0].as_str()
                    == clip_path
                        .with_extension("markers.json")
                        .file_name()
                        .and_then(|name| name.to_str())),
        "clip identity lost its marker reference"
    );
    ctx.evidence.put("clipMarkers", clip_markers);
    ctx.evidence.put("primaryMarkers", primary_markers);
    ctx.evidence.put("clipSourceOriginNs", begin_ns);
    stimulus.stop();
    let expect = VideoExpectation {
        codec: "h264",
        fps: 30.0,
        max_hold_s: 0.25,
        min_fresh_fraction: 0.5,
    };
    let primary = common::judge_video(ctx, &primary_path, &stimulus, &expect)?;
    common::judge_av_sync(ctx, &primary_path, 0, &primary)?;
    ctx.evidence
        .put("primaryTimeline", serde_json::to_value(&primary.timeline)?);
    let clip = common::judge_video(ctx, &clip_path, &stimulus, &expect)?;
    common::judge_av_sync(ctx, &clip_path, 0, &clip)?;
    for video in [&primary, &clip] {
        product_ensure!(
            video.probe["format"]["format_name"]
                .as_str()
                .is_some_and(|name| name.contains("matroska"))
                && media::streams(&video.probe, "audio").len() == 1
                && media::streams(&video.probe, "audio")[0]["codec_name"] == "opus",
            "the recording or clip lost its Matroska/Opus stream contract"
        );
    }
    let mut primary_pts = HashMap::new();
    for (pts, id) in &primary.timeline.samples {
        primary_pts.entry(*id).or_insert(*pts);
    }
    let mut origin = None;
    for (pts, id) in &clip.timeline.samples {
        let parent = primary_pts.get(id).ok_or_else(|| {
            Stop::fail(format!(
                "clip frame {id} is absent from the primary recording"
            ))
        })?;
        let shift = parent - pts;
        let first = *origin.get_or_insert(shift);
        product_ensure!(
            (shift - first).abs() <= 0.1,
            "clip timestamps do not preserve the primary frame timeline"
        );
    }
    // Adapter range labels are whole seconds, while decoded frame PTS retain fractions.
    product_ensure!(
        (clip.duration - (actual_end - actual_begin)).abs() <= 1.1
            && origin.is_some_and(|shift| (shift - actual_begin).abs() <= 1.1),
        "the decoded clip does not match its offered recording range"
    );
    product_ensure!(
        clip.frames
            .first()
            .is_some_and(|frame| frame.pts.abs() <= 0.1)
            && clip.duration > 0.0
            && clip.duration < primary.duration
            && primary.timeline.last_id > clip.timeline.last_id,
        "clip timestamps are not relative, or primary capture did not continue beyond its clipped range"
    );
    let primary_manifest = manifest(&primary.probe)?;
    let clip_manifest = manifest(&clip.probe)?;
    product_ensure!(
        clip_manifest["media_id"] == completed["assetId"]
            && clip_manifest["media_id"] != primary_manifest["media_id"]
            && clip_manifest["recording_id"] == primary_manifest["recording_id"]
            && clip_manifest["sources"] == primary_manifest["sources"]
            && clip_manifest["segment_origin_r_ns"] == 0
            && clip_manifest["derived_from_media_ids"]
                .as_array()
                .is_some_and(|ids| ids.contains(&primary_manifest["media_id"])),
        "clip metadata did not preserve recording/source identity and relative origin"
    );
    let primary_tracks = primary_manifest["tracks"]
        .as_array()
        .ok_or_else(|| Stop::fail("primary manifest has no tracks"))?;
    let clip_tracks = clip_manifest["tracks"]
        .as_array()
        .ok_or_else(|| Stop::fail("clip manifest has no tracks"))?;
    product_ensure!(
        primary_tracks.len() == 1 && clip_tracks.len() == 1,
        "the clip changed its audio track count"
    );
    for key in [
        "track_id",
        "source_ids",
        "routes",
        "fixed_average_divisor",
        "limiter",
        "packet_duration_ns",
        "preroll_ns",
    ] {
        product_ensure!(
            clip_tracks[0][key] == primary_tracks[0][key],
            "clip audio provenance changed {key}"
        );
    }
    // A copied Opus slice has a new decoder start, so its pre-skip can differ.
    for key in [
        "codec_id",
        "sample_rate",
        "channels",
        "bit_depth",
        "channel_layout",
    ] {
        product_ensure!(
            clip_tracks[0]["codec"][key] == primary_tracks[0]["codec"][key],
            "clip audio format changed {key}"
        );
    }
    product_ensure!(
        clip_tracks[0]["codec"]["codec_delay_samples"]
            .as_u64()
            .is_some_and(|samples| samples > 0),
        "clip metadata lost its Opus decoder priming"
    );
    let editor = accepted(
        &mut app,
        "clip.open",
        json!({ "assetId": completed["assetId"] }),
    )?;
    product_ensure!(
        editor["open"] == true
            && editor["clipPath"]
                .as_str()
                .is_some_and(|path| std::path::Path::new(path) == clip_path),
        "Edit opened a different asset"
    );
    let duration = editor["durationMs"]
        .as_f64()
        .ok_or_else(|| Stop::fail("Edit reported no clip duration"))?;
    product_ensure!(
        (duration / 1000.0 - clip.duration).abs() <= 0.2,
        "Edit duration contradicts the decoded clip"
    );
    let position = (duration / 2.0) as i64;
    let sought = accepted(&mut app, "edit.seek", json!({ "positionMs": position }))?;
    product_ensure!(
        sought["positionMs"]
            .as_f64()
            .is_some_and(|actual| (actual - position as f64).abs() <= 1.0),
        "the completed clip did not seek on its relative timeline"
    );
    ctx.evidence.put("primaryResult", primary_result);
    ctx.evidence
        .put("clipTimeline", serde_json::to_value(&clip.timeline)?);
    ctx.evidence.put("clipManifest", clip_manifest);
    ctx.evidence.put("clipEditor", sought);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unavailable_oracle_rejects_jobs_capability_drift_and_staging() {
        let directory = tempfile::tempdir().unwrap();
        let snapshot = json!({"productAvailable": false, "canRequest": false, "jobs": []});
        assert!(judge_unavailable(&snapshot, directory.path()).is_ok());
        for (key, value) in [
            ("productAvailable", json!(true)),
            ("canRequest", json!(true)),
            ("jobs", json!([{"jobId": "unexpected"}])),
            ("jobs", Value::Null),
        ] {
            let mut changed = snapshot.clone();
            changed[key] = value;
            assert!(matches!(
                judge_unavailable(&changed, directory.path()),
                Err(Stop::Fail(_))
            ));
        }
        std::fs::create_dir(directory.path().join("staging")).unwrap();
        assert!(matches!(
            judge_unavailable(&snapshot, directory.path()),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn unavailable_oracle_requires_a_typed_capability_refusal() {
        let refusal = Refusal {
            code: "unavailable".into(),
            message: "Recording Clips are unavailable in this product version".into(),
            requires: json!({"recordingClipsAvailable": true}),
            actual: json!({"recordingClipsAvailable": false}),
        };
        assert!(judge_refusal(Err(refusal.clone())).is_ok());
        assert!(matches!(judge_refusal(Ok(json!({}))), Err(Stop::Fail(_))));
        for field in ["code", "requires", "actual", "message"] {
            let mut changed = refusal.clone();
            match field {
                "code" => changed.code = "invalid_state".into(),
                "requires" => changed.requires = json!({"canRequestClip": true}),
                "actual" => changed.actual = json!({"recordingClipsAvailable": true}),
                _ => changed.message.clear(),
            }
            assert!(matches!(judge_refusal(Err(changed)), Err(Stop::Fail(_))));
        }
    }

    #[test]
    fn retained_positive_oracle_keeps_its_historical_identity() {
        let scenario = recording_live_scenario();
        assert_eq!(scenario.id, "clip.recording-live");
        assert_eq!(scenario.revision, 3);
        assert_eq!(scenario.tier, Tier::Required);
        assert_eq!(scenario.lane, Lane::Gpu);
        assert!(scenario.also.contains(&Lane::Preflight));
        assert!(!scenarios().iter().any(|s| s.id == scenario.id));
    }

    #[test]
    fn authored_marker_requires_exact_rebase_and_identity() {
        let primary = json!({"markers":[{"timeMs":20000,"type":"general","label":"Marker"}]});
        let clip = json!({"markers":[{"timeMs":16000,"type":"general","label":"Marker"}]});
        assert!(judge_clip_marker(&primary, &clip, 3_999_996_000).is_ok());
        let mut changed = clip.clone();
        changed["markers"][0]["timeMs"] = json!(15999);
        assert!(judge_clip_marker(&primary, &changed, 3_999_996_000).is_err());
        changed = clip.clone();
        changed["markers"][0]["label"] = json!("Other");
        assert!(judge_clip_marker(&primary, &changed, 3_999_996_000).is_err());
        assert!(judge_clip_marker(&primary, &json!({"markers":[]}), 3_999_996_000).is_err());
    }
}
