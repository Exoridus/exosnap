//! A candidate-bound recording, edit, export and restart journey.

use serde_json::{Value, json};
use std::collections::HashSet;
use std::path::Path;

use super::common::{self, Stimulus, StimulusOptions, secs};
use crate::bundle::sha256_file;
use crate::capability::Capability;
use crate::context::{Context, GracefulExit};
use crate::media;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "journey.record-edit-export-restart",
        revision: 5,
        title: "A recording can be edited, exported and reopened after restart",
        class: ScenarioClass::Contract,
        contract: "the candidate records a changing window, accepts a marker and pause/resume, closes the open edit session without confirmation on navigation to any other page, exports an ordered trim to a decodable file after reopening the editor clean, quits through its own shutdown with a recorded clean exit, and starts again with the same candidate identity, the settings it was given and no blocking surface",
        lane: Lane::Gpu,
        also: &[],
        tier: Tier::Required,
        requires: &[
            Capability::InteractiveDesktop,
            Capability::Wgc,
            Capability::Nvenc,
            Capability::Ffprobe,
        ],
        timeout: secs(360.0),
        run: journey,
    }]
}

fn accepted(app: &mut crate::context::App, command: &str, params: Value) -> Step<Value> {
    app.client
        .request(command, params, secs(30.0))?
        .map_err(|refusal| Stop::fail(format!("{command} refused: {refusal}")))
}

fn judge_edit(editor: &Value) -> Step {
    product_ensure!(editor["open"] == true, "the edit session did not open");
    let duration = editor["durationMs"].as_f64().unwrap_or(0.0);
    let start = editor["trimStartMs"].as_f64().unwrap_or(-1.0);
    let end = editor["trimEndMs"].as_f64().unwrap_or(-1.0);
    product_ensure!(
        duration > 0.0 && start >= 0.0 && start < end && end <= duration,
        "the editor reported an invalid trim {start}..{end} in a {duration} ms clip"
    );
    Ok(())
}

fn judge_export_state(state: &Value) -> Step {
    match state["exportState"].as_str() {
        Some("completed") => Ok(()),
        Some("failed") => Err(Stop::fail("the export failed")),
        other => Err(Stop::infra(format!(
            "export did not reach a terminal state: {other:?}"
        ))),
    }
}

fn decodable_video(path: &Path) -> Step {
    let probe = media::probe(path)?;
    product_ensure!(
        media::streams(&probe, "video").len() == 1,
        "{} does not contain one decodable video stream",
        path.display()
    );
    let (frames, _) = media::frame_packet_counts(path)?;
    product_ensure!(frames > 0, "{} decodes no video frames", path.display());
    Ok(())
}

fn video_files(dir: &Path) -> Step<HashSet<std::path::PathBuf>> {
    let entries = std::fs::read_dir(dir)?.collect::<Result<Vec<_>, _>>()?;
    Ok(entries
        .into_iter()
        .map(|entry| entry.path())
        .filter(|path| {
            path.is_file()
                && path
                    .extension()
                    .and_then(|part| part.to_str())
                    .is_some_and(|extension| {
                        extension.eq_ignore_ascii_case("mkv")
                            || extension.eq_ignore_ascii_case("mp4")
                    })
        })
        .collect())
}

fn journey(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let expected_hash = sha256_file(&ctx.product()?.exe)?.0;
    product_ensure!(
        app.identity()["executableSha256"]
            .as_str()
            .is_some_and(|hash| hash.eq_ignore_ascii_case(&expected_hash)),
        "the running process does not report the candidate executable hash"
    );
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
    let accent = common::configure(
        &mut app,
        &[(PERSISTED_APP_KEY.0, json!(PERSISTED_APP_KEY.1))],
    )?;
    infra_ensure!(
        accent.is_empty(),
        "the product did not accept {}={}: {accent:?}",
        PERSISTED_APP_KEY.0,
        PERSISTED_APP_KEY.1
    );
    let mut stimulus = Stimulus::start(ctx, StimulusOptions::default())?;
    common::select_window(&mut app, &stimulus.title)?;
    common::start_recording(&mut app)?;
    std::thread::sleep(secs(5.0));
    let live = app.call("pipeline.snapshot", json!({}))?;
    product_ensure!(
        live["valid"] == true && live["capture"]["targetFps"].as_f64().unwrap_or(0.0) > 0.0,
        "a running recording has no measured capture pipeline: {live}"
    );
    accepted(&mut app, "record.addMarker", json!({}))?;
    common::pause(&mut app)?;
    common::resume(&mut app)?;
    std::thread::sleep(secs(3.0));
    let result = common::stop_recording(&mut app)?;
    stimulus.stop();
    let original = common::output_path(&result)?;
    decodable_video(&original)?;
    product_ensure!(
        result["markerCount"].as_u64().unwrap_or(0) >= 1,
        "the completed recording lost its marker"
    );
    let editor = accepted(&mut app, "edit.open", json!({}))?;
    let duration = editor["durationMs"].as_f64().unwrap_or(0.0);
    product_ensure!(duration >= 3000.0, "the editor opened a {duration} ms clip");
    let start = (duration / 4.0) as i64;
    let end = (duration * 3.0 / 4.0) as i64;
    accepted(
        &mut app,
        "edit.seek",
        json!({ "positionMs": duration as i64 / 2 }),
    )?;
    accepted(&mut app, "edit.setTrimIn", json!({ "positionMs": start }))?;
    let trimmed = accepted(&mut app, "edit.setTrimOut", json!({ "positionMs": end }))?;
    judge_edit(&trimmed)?;
    let mut visits = Vec::new();
    for page in ["settings", "diagnostics", "logs", "about", "record"] {
        accepted(&mut app, "ui.navigate", json!({ "page": page }))?;
        let state = app.call("ui.getState", json!({}))?;
        visits.push(json!({ "page": page, "state": state.clone() }));
        judge_edit_closes_on_navigation(page, &state)?;
    }
    ctx.evidence.put("editNavigation", json!(visits));
    // product-spec.md: "Opening Edit again starts clean." The navigation above
    // discarded the first trim, so the export below is redone from a fresh
    // editor rather than reusing the session closed by the loop.
    let editor = accepted(&mut app, "edit.open", json!({}))?;
    let duration = editor["durationMs"].as_f64().unwrap_or(0.0);
    product_ensure!(
        duration >= 3000.0,
        "the reopened editor opened a {duration} ms clip"
    );
    let start = (duration / 4.0) as i64;
    let end = (duration * 3.0 / 4.0) as i64;
    accepted(
        &mut app,
        "edit.seek",
        json!({ "positionMs": duration as i64 / 2 }),
    )?;
    accepted(&mut app, "edit.setTrimIn", json!({ "positionMs": start }))?;
    let trimmed = accepted(&mut app, "edit.setTrimOut", json!({ "positionMs": end }))?;
    judge_edit(&trimmed)?;
    let video_before_export = video_files(&app.output)?;
    accepted(&mut app, "export.start", json!({}))?;
    let terminal = app
        .client
        .poll("ui.getState", json!({}), secs(180.0), |state| {
            state["exportState"] == "completed" || state["exportState"] == "failed"
        })?
        .ok_or_else(|| Stop::infra("export did not reach a terminal state within 180 s"))?;
    judge_export_state(&terminal)?;
    let exported: Vec<_> = video_files(&app.output)?
        .difference(&video_before_export)
        .cloned()
        .collect();
    product_ensure!(
        exported.len() == 1,
        "the export produced {} new files in the isolated output directory",
        exported.len()
    );
    decodable_video(&exported[0])?;
    let exported_probe = media::probe(&exported[0])?;
    let exported_duration = exported_probe["format"]["duration"]
        .as_str()
        .and_then(|value| value.parse::<f64>().ok())
        .unwrap_or(0.0);
    product_ensure!(
        exported_duration > 0.0 && exported_duration < duration / 1000.0,
        "the export duration {exported_duration} s does not reflect the trim from {} s",
        duration / 1000.0
    );
    accepted(&mut app, "edit.close", json!({}))?;
    ctx.evidence.put("recordResult", result);
    ctx.evidence
        .put("exportPath", exported[0].display().to_string());
    ctx.evidence.put("exportDurationSeconds", exported_duration);
    let exit = app.close_gracefully(secs(60.0))?;
    ctx.evidence.put("firstInstanceExitCode", exit.code);
    let mut restarted = ctx.launch(&[])?;
    product_ensure!(
        restarted.identity()["executableSha256"]
            .as_str()
            .is_some_and(|hash| hash.eq_ignore_ascii_case(&expected_hash)),
        "the restarted process does not report the candidate executable hash"
    );
    let state = restarted.call("ui.getState", json!({}))?;
    ctx.evidence.put("restartState", state.clone());
    judge_restart(&exit, &state)?;
    let mut persisted = Vec::new();
    for (key, expected) in persisted_settings() {
        let answer = restarted
            .client
            .request("settings.get", json!({ "key": key }), secs(15.0))?
            .map_err(|r| Stop::infra(format!("settings.get {key} was refused: {r}")))?;
        let value = answer["values"].get(key).cloned().unwrap_or(Value::Null);
        persisted.push(json!({ "key": key, "expected": expected.clone(), "value": value.clone() }));
        product_ensure!(
            value == expected,
            "after a clean restart {key} is {value}, it was set to {expected}"
        );
    }
    ctx.evidence.put("persistedSettings", json!(persisted));
    Ok(())
}

/// One application-level key the restart has to keep besides the recording
/// settings. Aqua is the default accent, so this value proves a write.
const PERSISTED_APP_KEY: (&str, &str) = ("app.accent", "magenta");

fn persisted_settings() -> Vec<(&'static str, Value)> {
    vec![
        ("video.container", json!("MKV")),
        ("video.videoCodec", json!("H.264")),
        ("video.frameRate", json!(30)),
        ("video.cfr", json!(true)),
        (PERSISTED_APP_KEY.0, json!(PERSISTED_APP_KEY.1)),
    ]
}

/// docs/product-spec.md: "Back, Escape, another clip or navigation closes it
/// and discards the unexported recipe without a dirty badge, confirmation or
/// draft. Returning to Record shows the normal Completed state." Edit is a
/// workspace over Record, not a session that survives leaving it.
fn judge_edit_closes_on_navigation(page: &str, state: &Value) -> Step {
    // The legacy "logs" request is an input alias for Diagnostics with the logs
    // subview; the canonical page is what the state reports.
    let expected = if page == "logs" { "diagnostics" } else { page };
    product_ensure!(
        state["page"] == expected,
        "ui.navigate {page} landed on {}",
        state["page"]
    );
    product_ensure!(
        state["editSession"] == "closed",
        "visiting {page} left the edit session {} instead of closing it",
        state["editSession"]
    );
    product_ensure!(
        state["editVisible"] == false,
        "on the {page} page editVisible is {}, expected the closed overlay to be hidden",
        state["editVisible"]
    );
    Ok(())
}

/// A normal restart is only judged after the first instance was proven to end
/// through the product's own shutdown; after a kill a crash surface is correct.
fn judge_restart(_after: &GracefulExit, state: &Value) -> Step {
    product_ensure!(
        state["blockingSurface"]
            .as_str()
            .is_none_or(|value| value.is_empty() || value == "none"),
        "restart after a clean shutdown opened a blocking surface: {}",
        state["blockingSurface"]
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn edit_oracle_requires_open_session_and_ordered_trim() {
        assert!(matches!(
            judge_edit(
                &json!({"open": false, "durationMs": 8000, "trimStartMs": 1000, "trimEndMs": 7000})
            ),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_edit(
                &json!({"open": true, "durationMs": 8000, "trimStartMs": 7000, "trimEndMs": 1000})
            ),
            Err(Stop::Fail(_))
        ));
        judge_edit(
            &json!({"open": true, "durationMs": 8000, "trimStartMs": 1000, "trimEndMs": 7000}),
        )
        .unwrap();
    }

    #[test]
    fn navigation_must_close_the_edit_session() {
        let closed = json!({"page": "settings", "editSession": "closed", "editVisible": false});
        judge_edit_closes_on_navigation("settings", &closed).unwrap();
        let closed_on_record =
            json!({"page": "record", "editSession": "closed", "editVisible": false});
        judge_edit_closes_on_navigation("record", &closed_on_record).unwrap();
        for (page, state) in [
            (
                "settings",
                json!({"page": "settings", "editSession": "open", "editVisible": false}),
            ),
            (
                "settings",
                json!({"page": "settings", "editSession": "closed", "editVisible": true}),
            ),
            (
                "record",
                json!({"page": "diagnostics", "editSession": "closed", "editVisible": false}),
            ),
        ] {
            assert!(matches!(
                judge_edit_closes_on_navigation(page, &state),
                Err(Stop::Fail(_))
            ));
        }
    }

    #[test]
    fn completed_export_is_required_after_export_start() {
        assert!(matches!(
            judge_export_state(&json!({"exportState": "failed"})),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_export_state(&json!({"exportState": "exporting"})),
            Err(Stop::Infra(_))
        ));
        judge_export_state(&json!({"exportState": "completed"})).unwrap();
    }
}
