//! Display observations available through the product control channel.

use serde_json::{Value, json};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use crate::capability::Capability;
use crate::context::Context;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::tools;
use crate::{infra_ensure, product_ensure};

use super::common::{self, secs};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "display.refresh-transaction",
            revision: 1,
            title: "A recording works at an applied display refresh rate",
            claim: "envctl applies and reads back an alternate refresh rate, the product records a decodable file, and envctl restores the exact original",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(180.0),
            run: refresh_transaction,
        },
        Scenario {
            id: "display.hdr-transaction",
            revision: 1,
            title: "HDR state agrees with the product and restores exactly",
            claim: "envctl applies and reads back HDR, the product reports HDR and records a decodable file, and envctl restores the exact original",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::D3d11,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(180.0),
            run: hdr_transaction,
        },
        Scenario {
            id: "display.scaling-minimum",
            revision: 1,
            title: "The main window respects its minimum size at a measured display scale",
            claim: "the product reports a display pixel ratio and a native main window at least 860 by 700 logical pixels",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(90.0),
            run: scaling_minimum,
        },
        Scenario {
            id: "display.mixed-preview-crossing",
            revision: 1,
            title: "Capture and preview continue across mixed HDR and SDR displays",
            claim: "each HDR and SDR display yields a decodable recording, and preview consumption continues after moving the main window across them",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::MultiMonitor,
                Capability::HdrDisplay,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(300.0),
            run: mixed_preview_crossing,
        },
    ]
}

fn envctl_path() -> Step<PathBuf> {
    let path = std::env::var_os("EXO_VERIFY_ENVCTL")
        .or_else(|| std::env::var_os("EXOSNAP_ENVCTL"))
        .map(PathBuf::from)
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

fn envctl(exe: &Path, args: &[&str]) -> Step<Value> {
    let output = tools::run(Command::new(exe).args(args), secs(30.0))?;
    let value: Value = serde_json::from_str(&output.stdout).map_err(|e| {
        Stop::infra(format!(
            "envctl returned invalid JSON: {e}; stderr: {}",
            output.stderr.trim()
        ))
    })?;
    infra_ensure!(
        output.success() == (value["ok"] == true),
        "envctl exit status and JSON outcome disagree: {value}"
    );
    Ok(value)
}

fn select_refresh(offered: &[i64], current: i64) -> Option<i64> {
    let rates: std::collections::BTreeSet<_> = offered.iter().copied().collect();
    rates.iter().rev().copied().find(|rate| {
        *rate != current && !rates.contains(&(rate - 1)) && !rates.contains(&(rate + 1))
    })
}

fn judge_restoration(restore: &Value, property: &str, wanted: &str) -> Step {
    infra_ensure!(
        restore["ok"] == true && restore["state"] == "Restored",
        "envctl did not finish restoration: {restore}"
    );
    infra_ensure!(
        restore["evidence"]["accepted"] == true,
        "envctl did not accept the restoration evidence: {restore}"
    );
    let entry = restore["evidence"]["properties"]
        .as_array()
        .and_then(|items| items.iter().find(|item| item["property"] == property))
        .ok_or_else(|| Stop::infra(format!("restore evidence lacks {property}")))?;
    infra_ensure!(
        entry["requested"] == wanted && entry["applied"] == wanted,
        "envctl did not read back the requested value for {property}: {entry}"
    );
    infra_ensure!(
        entry["restored"] == true && entry["before"].as_str() == entry["afterRestore"].as_str(),
        "envctl did not restore the exact original {property}: {entry}"
    );
    Ok(())
}

fn transact(
    ctx: &mut Context,
    property: &str,
    wanted: &str,
    body: impl FnOnce(&mut Context) -> Step,
) -> Step {
    let exe = envctl_path()?;
    let status = envctl(&exe, &["status"])?;
    infra_ensure!(
        status["ok"] == true && status["mutationAllowed"] == true,
        "envctl has an unresolved journal; restore it before this scenario: {status}"
    );
    let alias = property
        .split_once(':')
        .ok_or_else(|| Stop::infra("envctl property has no alias"))?
        .0;
    let snapshot = envctl(&exe, &["snapshot", "--aliases", alias])?;
    let observed = snapshot["properties"]
        .as_array()
        .and_then(|items| items.iter().find(|item| item["key"] == property))
        .ok_or_else(|| Stop::unavailable(format!("envctl has no bound property {property}")))?;
    if observed["capability"] != "ENV_MUTATE_SAFE" {
        return Err(Stop::unavailable(format!(
            "{property} is not a supported reversible environment setter"
        )));
    }
    infra_ensure!(
        observed["ok"] == true && observed["devicePresent"] == true,
        "envctl could not read the original value of {property}: {observed}"
    );
    ctx.evidence.put("environmentBefore", snapshot);
    let desired_path = ctx.scenario_dir.join("desired.json");
    let mut desired = serde_json::Map::new();
    desired.insert(property.to_string(), Value::String(wanted.to_string()));
    std::fs::write(
        &desired_path,
        serde_json::to_vec(&json!({"desired": desired}))?,
    )?;
    let guard = Command::new(&exe)
        .arg("--guard")
        .arg(std::process::id().to_string())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    let run_id = crate::control::new_run_id("env");
    let desired_arg = desired_path
        .to_str()
        .ok_or_else(|| Stop::infra("desired file path is not UTF-8"))?;
    let begun = envctl(
        &exe,
        &[
            "begin",
            "--scenario",
            property,
            "--run-id",
            &run_id,
            "--desired",
            desired_arg,
        ],
    );
    let outcome = match &begun {
        Ok(value) if value["ok"] == true && value["state"] == "Active" => std::panic::catch_unwind(
            std::panic::AssertUnwindSafe(|| body(ctx)),
        )
        .unwrap_or_else(|_| {
            Err(Stop::infra(
                "display scenario panicked after environment mutation",
            ))
        }),
        Ok(value) if value["ok"] == true => Err(Stop::infra(format!(
            "envctl begin did not reach Active: {value}"
        ))),
        Ok(value) => Err(Stop::unavailable(format!(
            "envctl could not apply {property}={wanted}: {value}"
        ))),
        Err(_) => Err(Stop::infra("envctl begin did not return a usable result")),
    };
    ctx.evidence.put(
        "environmentBegin",
        begun.as_ref().ok().cloned().unwrap_or(Value::Null),
    );
    let restore = envctl(&exe, &["restore"]);
    drop(guard);
    let restored = restore?;
    ctx.evidence.put("environmentRestore", restored.clone());
    if begun.as_ref().is_ok_and(|value| value["ok"] == true) {
        infra_ensure!(
            begun.as_ref().unwrap()["transactionId"]
                .as_str()
                .is_some_and(|id| !id.is_empty())
                && begun.as_ref().unwrap()["transactionId"]
                    == restored["evidence"]["transactionId"],
            "envctl restore evidence does not belong to this transaction"
        );
        judge_restoration(&restored, property, wanted)?;
    } else {
        infra_ensure!(
            restored["ok"] == true && restored["pending"].as_array().is_some_and(Vec::is_empty),
            "envctl begin failed and restoration is incomplete: {restored}"
        );
    }
    outcome
}

fn display_alias() -> String {
    std::env::var("EXO_VERIFY_DISPLAY_ALIAS").unwrap_or_else(|_| "display.main-hdr".into())
}

fn refresh_transaction(ctx: &mut Context) -> Step {
    let exe = envctl_path()?;
    let alias = display_alias();
    let modes = envctl(&exe, &["list-modes", "--alias", &alias])?;
    if modes["ok"] != true {
        return Err(Stop::unavailable(format!(
            "display modes for {alias} are unavailable: {modes}"
        )));
    }
    let display = modes["displays"]
        .as_array()
        .and_then(|items| items.first())
        .ok_or_else(|| Stop::unavailable(format!("no display is bound to alias {alias}")))?;
    let current = display["current"]["refreshHz"]
        .as_i64()
        .ok_or_else(|| Stop::infra("envctl returned no current display refresh rate"))?;
    let offered: Vec<i64> = display["modes"]
        .as_array()
        .ok_or_else(|| Stop::infra("envctl returned no display modes"))?
        .iter()
        .filter_map(|mode| mode["refreshHz"].as_i64())
        .collect();
    let target = select_refresh(&offered, current).ok_or_else(|| {
        Stop::unavailable("no alternate, untwinned refresh rate can be read back exactly")
    })?;
    let property = format!("{alias}:refresh-hz");
    let wanted = target.to_string();
    let device = display["gdiName"]
        .as_str()
        .ok_or_else(|| Stop::infra("envctl did not identify the bound display"))?
        .to_string();
    ctx.evidence.put("displayModes", modes);
    transact(ctx, &property, &wanted, |ctx| {
        record_display(ctx, &device, false)
    })
}

fn hdr_transaction(ctx: &mut Context) -> Step {
    let alias = display_alias();
    let exe = envctl_path()?;
    let modes = envctl(&exe, &["list-modes", "--alias", &alias])?;
    if modes["ok"] != true {
        return Err(Stop::unavailable(format!(
            "display alias {alias} cannot be resolved: {modes}"
        )));
    }
    let device = modes["displays"]
        .as_array()
        .and_then(|items| items.first())
        .and_then(|display| display["gdiName"].as_str())
        .filter(|name| !name.is_empty())
        .ok_or_else(|| Stop::infra("envctl did not identify the bound display"))?
        .to_string();
    let property = format!("{alias}:hdr");
    ctx.evidence.put("displayModes", modes);
    transact(ctx, &property, "on", |ctx| {
        record_display(ctx, &device, true)
    })
}

fn judge_hdr(environment: &Value, device: &str) -> Step {
    let screen = environment["displays"]["screens"]
        .as_array()
        .and_then(|items| items.iter().find(|screen| screen["device"] == device))
        .ok_or_else(|| Stop::infra(format!("product environment has no bound display {device}")))?;
    product_ensure!(
        screen["hdrActive"] == true,
        "envctl applied HDR on {device} but the product reports {}",
        screen["hdrActive"]
    );
    Ok(())
}

fn record_display(ctx: &mut Context, device: &str, hdr_expected: bool) -> Step {
    let mut app = ctx.launch(&[])?;
    let environment = app.call("environment.snapshot", json!({}))?;
    if hdr_expected {
        judge_hdr(&environment, device)?;
    }
    common::select_display(&mut app, device)?;
    let (result, file, wall) = common::record_for(&mut app, 8.0)?;
    let probe = crate::media::probe(&file)?;
    product_ensure!(
        !crate::media::streams(&probe, "video").is_empty(),
        "the display recording has no video stream"
    );
    let frames = crate::media::luma_frames(&file, 480)?;
    product_ensure!(
        frames.len() >= 60,
        "the display recording decoded only {} frames",
        frames.len()
    );
    let decoded_span = frames.last().unwrap().pts - frames.first().unwrap().pts;
    product_ensure!(
        decoded_span >= 5.0,
        "the display recording decoded only {decoded_span:.2} s of video"
    );
    ctx.evidence.put("displayEnvironment", environment);
    ctx.evidence.put("recordResult", result);
    ctx.evidence.put("ffprobe", probe);
    ctx.evidence.put("recordWallSeconds", wall);
    ctx.evidence.put("decodedFrames", frames.len() as u64);
    ctx.evidence.put("decodedSpanSeconds", decoded_span);
    app.kill_for_cleanup()?;
    Ok(())
}

fn scaling_minimum(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let windows = app.call("windows.snapshot", json!({}))?;
    ctx.evidence
        .put("displays", environment["displays"].clone());
    ctx.evidence.put("windows", windows.clone());
    judge_scaling(&environment, &windows)
}

fn judge_scaling(environment: &Value, windows: &Value) -> Step {
    let screens = environment["displays"]["screens"]
        .as_array()
        .ok_or_else(|| Stop::infra("the display snapshot has no screens array"))?;
    infra_ensure!(
        !screens.is_empty(),
        "the display snapshot reports no screens"
    );
    infra_ensure!(
        screens.iter().all(|screen| screen["devicePixelRatio"]
            .as_f64()
            .is_some_and(|ratio| ratio.is_finite() && ratio > 0.0)),
        "at least one display has no measured device pixel ratio"
    );
    let main = windows["windows"]
        .as_array()
        .and_then(|items| items.iter().find(|window| window["role"] == "main"))
        .ok_or_else(|| Stop::fail("the product reports no main window"))?;
    infra_ensure!(
        main["nativeWindowCreated"] == true,
        "the main window has no native geometry yet"
    );
    let width = main["native"]["width"]
        .as_i64()
        .ok_or_else(|| Stop::infra("the main window has no native width"))?;
    let height = main["native"]["height"]
        .as_i64()
        .ok_or_else(|| Stop::infra("the main window has no native height"))?;
    product_ensure!(
        width >= 860 && height >= 700,
        "main window is {width}x{height}, below the 860x700 minimum"
    );
    Ok(())
}

fn mixed_preview_crossing(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let screens = environment["displays"]["screens"]
        .as_array()
        .ok_or_else(|| Stop::infra("the display snapshot has no screens array"))?;
    if screens.len() < 2 {
        return Err(Stop::unavailable(
            "the machine has fewer than two attached displays",
        ));
    }
    let hdr_count = screens.iter().filter(|s| s["hdrActive"] == true).count();
    let sdr_count = screens.iter().filter(|s| s["hdrActive"] == false).count();
    if hdr_count == 0 || sdr_count == 0 {
        return Err(Stop::unavailable(
            "the attached displays do not report both HDR and SDR states",
        ));
    }
    let before = app.call("windows.snapshot", json!({}))?;
    let current = before["windows"]
        .as_array()
        .and_then(|items| items.iter().find(|w| w["role"] == "main"))
        .and_then(|w| w["screen"].as_str())
        .ok_or_else(|| Stop::infra("the main window has no reported screen"))?;
    // The window reports Qt's screen name, which window.moveToScreen takes; the
    // capture target is selected by the same screen's display device.
    let current_screen = screens
        .iter()
        .find(|s| s["name"] == current)
        .ok_or_else(|| Stop::infra(format!("the main window's screen {current} is not listed")))?;
    let current_hdr = current_screen["hdrActive"]
        .as_bool()
        .ok_or_else(|| Stop::infra("the main window's current display has no HDR state"))?;
    let current_device = common::screen_device(current_screen)?;
    let target_screen = screens
        .iter()
        .find(|s| s["name"] != current && s["hdrActive"].as_bool() == Some(!current_hdr))
        .ok_or_else(|| Stop::unavailable("no display of the opposite HDR state can be selected"))?;
    let target = target_screen["name"]
        .as_str()
        .ok_or_else(|| Stop::infra("the opposite-HDR display has no screen name"))?;
    let target_device = common::screen_device(target_screen)?;
    common::select_display(&mut app, &current_device)?;
    let baseline = watch_preview(&mut app, secs(10.0))?;
    infra_ensure!(
        baseline,
        "the preview consumed no frame before the display crossing"
    );
    let first = record_mixed_display(&mut app, &current_device)?;
    app.client
        .request("window.moveToScreen", json!({"screen": target}), secs(15.0))?
        .map_err(|refusal| Stop::fail(format!("window.moveToScreen refused: {refusal}")))?;
    let mut samples = Vec::new();
    for index in 0..6 {
        if index > 0 {
            std::thread::sleep(Duration::from_millis(250));
        }
        samples.push(app.call("preview.snapshot", json!({}))?);
    }
    let after = app.call("windows.snapshot", json!({}))?;
    ctx.evidence.put("previewSamples", json!(samples));
    ctx.evidence.put("windowsAfter", after.clone());
    product_ensure!(
        after["windows"]
            .as_array()
            .and_then(|items| items.iter().find(|w| w["role"] == "main"))
            .and_then(|w| w["screen"].as_str())
            == Some(target),
        "the main window did not reach display {target}"
    );
    judge_preview_progress(&samples)?;
    let second = record_mixed_display(&mut app, &target_device)?;
    judge_mixed_recordings(&first, &second)?;
    ctx.evidence.put("mixedDisplayRecordings", json!([
        {"display": first.display, "outputPath": first.file, "decodedFrames": first.frames, "decodedSpanSeconds": first.span_seconds},
        {"display": second.display, "outputPath": second.file, "decodedFrames": second.frames, "decodedSpanSeconds": second.span_seconds}
    ]));
    ctx.keep(&first.file);
    ctx.keep(&second.file);
    Ok(())
}

struct DisplayRecording {
    display: String,
    file: PathBuf,
    frames: usize,
    span_seconds: f64,
}

fn record_mixed_display(app: &mut crate::context::App, display: &str) -> Step<DisplayRecording> {
    common::select_display(app, display)?;
    let (_, file, _) = common::record_for(app, 8.0)?;
    let probe = crate::media::probe(&file)?;
    product_ensure!(
        crate::media::streams(&probe, "video").len() == 1,
        "recording on {display} does not have exactly one video stream"
    );
    let decoded = crate::media::luma_frames(&file, 480)?;
    let span_seconds = decoded
        .last()
        .zip(decoded.first())
        .map(|(last, first)| last.pts - first.pts)
        .unwrap_or(0.0);
    Ok(DisplayRecording {
        display: display.to_string(),
        file,
        frames: decoded.len(),
        span_seconds,
    })
}

fn judge_mixed_recordings(first: &DisplayRecording, second: &DisplayRecording) -> Step {
    infra_ensure!(
        first.display != second.display,
        "the mixed-display recordings target the same display"
    );
    infra_ensure!(
        first.file != second.file,
        "both display recordings refer to the same output file"
    );
    for recording in [first, second] {
        product_ensure!(
            recording.frames >= 60 && recording.span_seconds >= 5.0,
            "recording on {} decoded only {} frames spanning {:.2} s",
            recording.display,
            recording.frames,
            recording.span_seconds
        );
    }
    Ok(())
}

fn watch_preview(app: &mut crate::context::App, timeout: Duration) -> Step<bool> {
    let first = app.call("preview.snapshot", json!({}))?;
    let start = first["consumedFrames"]
        .as_f64()
        .ok_or_else(|| Stop::infra("preview has no consumedFrames counter"))?;
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(200));
        let current = app.call("preview.snapshot", json!({}))?;
        let consumed = current["consumedFrames"]
            .as_f64()
            .ok_or_else(|| Stop::infra("preview lost its consumedFrames counter"))?;
        if consumed > start {
            return Ok(true);
        }
    }
    Ok(false)
}

fn judge_preview_progress(samples: &[Value]) -> Step {
    infra_ensure!(
        samples.len() >= 2,
        "preview progress needs at least two observations"
    );
    let first = samples.first().unwrap();
    let last = samples.last().unwrap();
    let consumed_start = first["consumedFrames"]
        .as_f64()
        .ok_or_else(|| Stop::infra("preview has no first consumedFrames counter"))?;
    let consumed_end = last["consumedFrames"]
        .as_f64()
        .ok_or_else(|| Stop::infra("preview has no final consumedFrames counter"))?;
    infra_ensure!(
        last["updateGate"]["renderPasses"].as_f64().is_some(),
        "preview has no renderPasses counter"
    );
    if consumed_end > consumed_start {
        return Ok(());
    }
    if samples
        .iter()
        .all(|sample| sample["updateGate"]["owed"] == true)
    {
        return Err(Stop::fail(
            "a published preview frame stayed unrendered across the display crossing",
        ));
    }
    Err(Stop::infra(
        "the preview consumed no frame after crossing and no persistent publish debt was observed",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn scaling_requires_a_measured_display_ratio() {
        let result = judge_scaling(
            &json!({"displays":{"screens":[{"name":"DISPLAY1"}]}}),
            &json!({"windows":[{"role":"main","nativeWindowCreated":true,"native":{"width":860,"height":700}}]}),
        );
        assert!(matches!(result, Err(Stop::Infra(_))));
    }

    #[test]
    fn scaling_rejects_a_main_window_below_its_minimum() {
        let result = judge_scaling(
            &json!({"displays":{"screens":[{"devicePixelRatio":1.5}]}}),
            &json!({"windows":[{"role":"main","nativeWindowCreated":true,"native":{"width":859,"height":700}}]}),
        );
        assert!(matches!(result, Err(Stop::Fail(_))));
    }

    #[test]
    fn scaling_accepts_the_minimum_at_a_reported_scale() {
        judge_scaling(
            &json!({"displays":{"screens":[{"devicePixelRatio":1.5}]}}),
            &json!({"windows":[{"role":"main","nativeWindowCreated":true,"native":{"width":860,"height":700}}]}),
        )
        .unwrap();
    }

    #[test]
    fn preview_progress_requires_consumption_after_crossing() {
        let samples = vec![
            json!({"consumedFrames":10,"updateGate":{"renderPasses":5,"owed":true}}),
            json!({"consumedFrames":10,"updateGate":{"renderPasses":5,"owed":true}}),
        ];
        assert!(matches!(
            judge_preview_progress(&samples),
            Err(Stop::Fail(_))
        ));
        let samples = vec![
            json!({"consumedFrames":10,"updateGate":{"renderPasses":5,"owed":false}}),
            json!({"consumedFrames":11,"updateGate":{"renderPasses":6,"owed":true}}),
        ];
        judge_preview_progress(&samples).unwrap();
    }

    #[test]
    fn preview_progress_without_published_work_is_infrastructure() {
        let samples = vec![
            json!({"consumedFrames":10,"updateGate":{"renderPasses":5,"owed":false}}),
            json!({"consumedFrames":10,"updateGate":{"renderPasses":5,"owed":false}}),
        ];
        assert!(matches!(
            judge_preview_progress(&samples),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn preview_progress_with_missing_counters_is_infrastructure() {
        let samples = vec![
            json!({"consumedFrames":10,"updateGate":{"owed":true}}),
            json!({"consumedFrames":11,"updateGate":{"owed":false}}),
        ];
        assert!(matches!(
            judge_preview_progress(&samples),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn refresh_selection_avoids_nominal_twins() {
        assert_eq!(select_refresh(&[59, 60, 120, 144], 120), Some(144));
        assert_eq!(select_refresh(&[59, 60, 120, 144], 144), Some(120));
        assert_eq!(select_refresh(&[59, 60], 59), None);
    }

    #[test]
    fn transaction_evidence_requires_exact_restore() {
        let good = json!({"ok":true,"state":"Restored","evidence":{"accepted":true,"properties":[
            {"property":"display.main-hdr:hdr","before":"off","requested":"on","applied":"on","afterRestore":"off","restored":true}
        ]}});
        judge_restoration(&good, "display.main-hdr:hdr", "on").unwrap();
        let mut wrong = good.clone();
        wrong["evidence"]["properties"][0]["afterRestore"] = json!("on");
        assert!(matches!(
            judge_restoration(&wrong, "display.main-hdr:hdr", "on"),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn hdr_must_be_reported_on_the_bound_display() {
        // Twin panels share a friendly name; only the device tells them apart.
        let environment = json!({"displays":{"screens":[
            {"name":"27GL850","device":r"\\.\DISPLAY1","hdrActive":false},
            {"name":"27GL850","device":r"\\.\DISPLAY2","hdrActive":true}
        ]}});
        assert!(matches!(
            judge_hdr(&environment, r"\\.\DISPLAY1"),
            Err(Stop::Fail(_))
        ));
        judge_hdr(&environment, r"\\.\DISPLAY2").unwrap();
        assert!(matches!(
            judge_hdr(&environment, "27GL850"),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn mixed_display_requires_distinct_decoded_recordings() {
        let first = DisplayRecording {
            display: "DISPLAY1".into(),
            file: PathBuf::from("first.mkv"),
            frames: 120,
            span_seconds: 6.0,
        };
        let second = DisplayRecording {
            display: "DISPLAY2".into(),
            file: PathBuf::from("second.mkv"),
            frames: 120,
            span_seconds: 6.0,
        };
        judge_mixed_recordings(&first, &second).unwrap();
        let same_output = DisplayRecording {
            file: first.file.clone(),
            ..second
        };
        assert!(matches!(
            judge_mixed_recordings(&first, &same_output),
            Err(Stop::Infra(_))
        ));
        let short = DisplayRecording {
            frames: 2,
            file: PathBuf::from("short.mkv"),
            ..same_output
        };
        assert!(matches!(
            judge_mixed_recordings(&first, &short),
            Err(Stop::Fail(_))
        ));
    }
}
