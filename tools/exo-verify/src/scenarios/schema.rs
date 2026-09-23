//! Live Verify field contract checked against the candidate's real emitters.

use serde_json::{Value, json};
use std::collections::BTreeMap;

use super::common::{self, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

type Field = (&'static str, &'static str);

const IDLE: &[Field] = &[
    ("app.identity", "productVersion"),
    ("app.identity", "executableSha256"),
    ("environment.snapshot", "present.optIn"),
    ("environment.snapshot", "present.elevated"),
    ("environment.snapshot", "present.available"),
    ("environment.snapshot", "present.availability"),
    ("environment.snapshot", "displays.screens[].name"),
    ("environment.snapshot", "displays.screens[].primary"),
    ("environment.snapshot", "displays.screens[].hdrActive"),
    (
        "environment.snapshot",
        "displays.screens[].devicePixelRatio",
    ),
    ("windows.snapshot", "windows[].role"),
    ("windows.snapshot", "windows[].nativeWindowCreated"),
    ("preview.snapshot", "active"),
    ("preview.snapshot", "frameReady"),
    ("preview.snapshot", "updateGate.owed"),
    ("preview.snapshot", "updateGate.renderPasses"),
    ("record.snapshot", "systemAudioEnabled"),
    ("overlay.snapshot", "overlays[].visible"),
    ("notifications.snapshot", "entries[].sequence"),
    ("notifications.snapshot", "entries[].title"),
    ("notifications.snapshot", "entries[].body"),
    ("update.getState", "updateAvailable"),
    ("update.getState", "state"),
    ("update.getState", "blocker"),
    ("update.getState", "currentVersion"),
    ("update.getState", "updaterLaunch.controlRunId"),
    ("update.getState", "updaterLaunch.controlPipe"),
];

const RECORDING: &[Field] = &[
    ("pipeline.snapshot", "valid"),
    ("pipeline.snapshot", "lifecycle"),
    ("pipeline.snapshot", "capture.actualFps"),
    ("pipeline.snapshot", "sourcePresentation.presentMode"),
    ("pipeline.snapshot", "sourcePresentation.modeAvailability"),
    ("pipeline.snapshot", "audio.active"),
    ("pipeline.snapshot", "audio.sourceDegraded"),
    ("pipeline.snapshot", "audio.degradedSources"),
    ("pipeline.snapshot", "avTiming.avDriftMs"),
    ("pipeline.snapshot", "avTiming.avDriftAvailability"),
];

const RESULT: &[Field] = &[
    ("record.result", "succeeded"),
    ("record.result", "outputPath"),
];

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "schema.live-verify-fields",
        revision: 1,
        title: "Live Verify fields consumed by release scenarios are emitted",
        claim: "every declared field exists in idle, measured recording and completed-result snapshots, including the shape of every nonempty collection",
        lane: Lane::Gpu,
        also: &[],
        tier: Tier::Required,
        requires: &[
            Capability::InteractiveDesktop,
            Capability::DxgiDuplication,
            Capability::Nvenc,
            Capability::AudioRender,
        ],
        timeout: secs(240.0),
        run: field_contract,
    }]
}

fn resolve_path<'a>(snapshot: &'a Value, path: &str) -> Step<&'a Value> {
    let mut current = snapshot;
    for segment in path.split('.') {
        if let Some(key) = segment.strip_suffix("[]") {
            current = current
                .get(key)
                .ok_or_else(|| Stop::fail(format!("missing field {path} at {key}")))?;
            let array = current
                .as_array()
                .ok_or_else(|| Stop::fail(format!("{path}: {key} is not an array")))?;
            current = array.first().ok_or_else(|| {
                Stop::unavailable(format!(
                    "{path}: {key} is empty, so element shape is unchecked"
                ))
            })?;
        } else {
            current = current
                .get(segment)
                .ok_or_else(|| Stop::fail(format!("missing field {path} at {segment}")))?;
        }
    }
    Ok(current)
}

fn check_stage(app: &mut App, fields: &[Field], stage: &str, ctx: &mut Context) -> Step<usize> {
    let mut snapshots = BTreeMap::<&str, Value>::new();
    for (command, _) in fields {
        if !snapshots.contains_key(command) {
            snapshots.insert(command, app.call(command, json!({}))?);
        }
    }
    ctx.evidence.put(stage, serde_json::to_value(&snapshots)?);
    for (command, path) in fields {
        resolve_path(&snapshots[command], path).map_err(|stop| match stop {
            Stop::Fail(message) => Stop::fail(format!("{stage} {command}.{path}: {message}")),
            Stop::Unavailable(message) => {
                Stop::unavailable(format!("{stage} {command}.{path}: {message}"))
            }
            other => other,
        })?;
    }
    Ok(fields.len())
}

fn field_contract(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    app.client
        .request(
            "notification.raise",
            json!({
                "type": "windowCaptureStalled",
                "title": "Field contract probe",
                "body": "Synthetic entry to inspect notification element fields"
            }),
            secs(15.0),
        )?
        .map_err(|refusal| Stop::infra(format!("notification setup refused: {refusal}")))?;
    app.client
        .poll(
            "notifications.snapshot",
            json!({}),
            secs(10.0),
            |snapshot| {
                snapshot["entries"]
                    .as_array()
                    .is_some_and(|entries| !entries.is_empty())
            },
        )?
        .ok_or_else(|| Stop::infra("synthetic notification never reached the hub"))?;
    let mut checked = check_stage(&mut app, IDLE, "idle", ctx)?;
    common::configure_exact(
        &mut app,
        &[
            ("audio.systemEnabled", json!(true)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
        ],
    )?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let monitor = environment["displays"]["screens"]
        .as_array()
        .and_then(|screens| screens.iter().find(|screen| screen["primary"] == true))
        .and_then(|screen| screen["name"].as_str())
        .ok_or_else(|| Stop::unavailable("the product reports no primary display"))?
        .to_string();
    common::select_display(&mut app, &monitor)?;
    common::start_recording(&mut app)?;
    std::thread::sleep(secs(4.0));
    let live = app.call("pipeline.snapshot", json!({}))?;
    infra_ensure!(
        live["valid"] == true,
        "the recording pipeline published no valid sample"
    );
    checked += check_stage(&mut app, RECORDING, "recording", ctx)?;
    let result = common::stop_recording(&mut app)?;
    product_ensure!(
        result["succeeded"] == true,
        "the field-contract recording failed"
    );
    checked += check_stage(&mut app, RESULT, "result", ctx)?;
    ctx.evidence.put("checkedFields", checked);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn nested_array_paths_are_resolved() {
        let value = json!({"displays": {"screens": [{"name": "one", "primary": true}]}});
        assert!(resolve_path(&value, "displays.screens[].name").is_ok());
    }

    #[test]
    fn empty_arrays_cannot_pass_an_element_contract() {
        let value = json!({"displays": {"screens": []}});
        assert!(matches!(
            resolve_path(&value, "displays.screens[].name"),
            Err(Stop::Unavailable(_))
        ));
    }

    #[test]
    fn missing_fields_are_product_failures() {
        let value = json!({"present": {"optIn": true}});
        assert!(matches!(
            resolve_path(&value, "present.availability"),
            Err(Stop::Fail(_))
        ));
    }
}
