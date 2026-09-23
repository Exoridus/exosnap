//! Operator judged desktop composition, with structural product state as setup evidence.

use serde_json::{Value, json};
use std::process::Command;

use super::common::{self, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "visual.capture-overlays",
            revision: 1,
            title: "Capture overlays stay legible under light and dark Windows appearances",
            claim: "visible native capture overlays compose with fixed dark surfaces and legible text under both Windows app appearances, judged on the desktop by an operator",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::Windows,
                Capability::InteractiveDesktop,
                Capability::DxgiDuplication,
                Capability::Nvenc,
                Capability::Operator,
            ],
            timeout: secs(600.0),
            run: capture_overlays,
        },
        Scenario {
            id: "visual.notification-severity",
            revision: 1,
            title: "Notifications show a glyph and tint matching each severity",
            claim: "synthetic success, caution, error and info notifications reach the hub and desktop, where an operator judges their glyphs and tints",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::Windows,
                Capability::InteractiveDesktop,
                Capability::Operator,
            ],
            timeout: secs(360.0),
            run: notification_severity,
        },
    ]
}

fn accepted(app: &mut App, command: &str, params: Value) -> Step<Value> {
    app.client
        .request(command, params, secs(15.0))?
        .map_err(|refusal| {
            Stop::infra(format!(
                "{command} was refused during visual setup: {refusal}"
            ))
        })
}

fn appearance() -> Step<bool> {
    let output = crate::tools::run(
        Command::new("reg.exe").args([
            "query",
            r"HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
            "/v",
            "AppsUseLightTheme",
        ]),
        secs(10.0),
    )?;
    if !output.success() {
        return Err(Stop::unavailable("Windows apps appearance cannot be read"));
    }
    let value = output.stdout.lines().find_map(|line| {
        let parts: Vec<_> = line.split_whitespace().collect();
        (parts.len() >= 3 && parts[0] == "AppsUseLightTheme")
            .then(|| parts.last().copied().unwrap_or(""))
    });
    match value {
        Some("0x0") => Ok(false),
        Some("0x1") => Ok(true),
        _ => Err(Stop::unavailable(
            "Windows apps appearance has an unknown registry value",
        )),
    }
}

fn judge_overlays(snapshot: &Value, expected: &[&str]) -> Step {
    let overlays = snapshot["overlays"]
        .as_array()
        .ok_or_else(|| Stop::infra("overlay.snapshot has no overlays array"))?;
    for name in expected {
        infra_ensure!(
            overlays.iter().any(|overlay| {
                overlay["objectName"] == *name
                    && overlay["visible"] == true
                    && overlay["nativeWindowCreated"] == true
            }),
            "{name} has no visible native window to judge"
        );
    }
    Ok(())
}

fn sample_overlays(ctx: &mut Context, app: &mut App, name: &str, expected: &[&str]) -> Step {
    let snapshot = app.call("overlay.snapshot", json!({}))?;
    ctx.evidence.put(name, snapshot.clone());
    judge_overlays(&snapshot, expected)?;
    if !ctx.ask(&format!(
        "Can you see {} on the desktop?",
        expected.join(", ")
    ))? {
        return Err(Stop::unavailable(
            "the operator cannot see the overlays on the desktop",
        ));
    }
    product_ensure!(
        ctx.ask("Do the visible capture overlays have fixed dark surfaces, legible text and correct state colours?")?,
        "the operator judged a capture overlay's desktop appearance incorrect"
    );
    Ok(())
}

fn inspect_appearances(ctx: &mut Context, app: &mut App, prefix: &str, expected: &[&str]) -> Step {
    let original = appearance()?;
    let first_name = format!("{prefix}{}", if original { "Light" } else { "Dark" });
    let second_name = format!("{prefix}{}", if original { "Dark" } else { "Light" });
    let first = sample_overlays(ctx, app, &first_name, expected);
    let second = (|| -> Step {
        if first.is_err() {
            return Ok(());
        }
        let target = if original { "dark" } else { "light" };
        if !ctx.ask(&format!(
            "Switch Windows app appearance to {target}, then answer yes when it is applied."
        ))? {
            Err(Stop::unavailable(
                "the operator did not switch Windows appearance",
            ))
        } else if appearance()? == original {
            Err(Stop::unavailable("Windows apps appearance did not change"))
        } else {
            sample_overlays(ctx, app, &second_name, expected)
        }
    })();
    if !matches!(appearance(), Ok(current) if current == original) {
        ctx.announce("Restore the original Windows app appearance now.");
        let _ = ctx.ask("Have you restored the original Windows app appearance?");
    }
    if !matches!(appearance(), Ok(current) if current == original) {
        ctx.cleanup_failed = true;
        return Err(Stop::infra("Windows apps appearance was not restored"));
    }
    first?;
    second
}

fn capture_overlays(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("app.showRecordingOverlay", json!(true)),
            ("app.showDiagnosticsOverlay", json!(true)),
            ("app.showQuickControls", json!(true)),
            ("audio.systemEnabled", json!(false)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
        ],
    )?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let monitor = environment["displays"]["screens"]
        .as_array()
        .and_then(|screens| screens.iter().find(|screen| screen["primary"] == true))
        .and_then(|screen| screen["name"].as_str())
        .ok_or_else(|| Stop::unavailable("the product reports no primary monitor"))?
        .to_string();
    common::select_display(&mut app, &monitor)?;
    common::start_recording(&mut app)?;
    accepted(
        &mut app,
        "notification.raise",
        json!({
            "type": "windowCaptureStalled",
            "title": "Visual overlay check",
            "body": "Synthetic toast for desktop appearance inspection"
        }),
    )?;
    inspect_appearances(
        ctx,
        &mut app,
        "recordingOverlays",
        &[
            "quickOverlayRecording",
            "quickOverlayDiagnostics",
            "quickOverlayQuickControls",
        ],
    )?;
    common::stop_recording(&mut app)?;
    app.close()?;
    let mut countdown = ctx.launch(&["--overlay-visual-state", "hud-countdown"])?;
    let ready = countdown
        .client
        .poll("overlay.snapshot", json!({}), secs(10.0), |snapshot| {
            judge_overlays(snapshot, &["quickOverlayCountdown"]).is_ok()
        })?
        .ok_or_else(|| Stop::infra("the countdown overlay did not create a native window"))?;
    ctx.evidence.put("countdownReady", ready);
    inspect_appearances(
        ctx,
        &mut countdown,
        "countdownOverlay",
        &["quickOverlayCountdown"],
    )?;
    Ok(())
}

fn judge_notification(snapshot: &Value, sequence: f64) -> Step {
    let entries = snapshot["entries"]
        .as_array()
        .ok_or_else(|| Stop::infra("notifications.snapshot has no entries array"))?;
    let entry = entries
        .iter()
        .find(|entry| entry["sequence"].as_f64() == Some(sequence))
        .ok_or_else(|| Stop::infra(format!("notification {sequence} never reached the hub")))?;
    product_ensure!(
        entry["severity"]
            .as_str()
            .is_some_and(|word| !word.is_empty()),
        "notification {sequence} has no severity word"
    );
    let toasts = snapshot["activeToasts"]
        .as_array()
        .ok_or_else(|| Stop::infra("notifications.snapshot has no activeToasts array"))?;
    infra_ensure!(
        toasts
            .iter()
            .any(|toast| toast["sequence"].as_f64() == Some(sequence)),
        "notification {sequence} has no active desktop toast"
    );
    Ok(())
}

fn notification_severity(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    accepted(&mut app, "notificationHub.open", json!({}))?;
    for (kind, severity) in [
        ("saved", "success"),
        ("windowCaptureStalled", "caution"),
        ("unexpectedStop", "error"),
        ("presetSwitched", "info"),
    ] {
        let title = format!("Visual {severity} check");
        let raised = accepted(
            &mut app,
            "notification.raise",
            json!({
                "type": kind,
                "title": title,
                "body": "Synthetic notification for visual inspection"
            }),
        )?;
        let sequence = raised["sequence"]
            .as_f64()
            .ok_or_else(|| Stop::infra("notification.raise returned no sequence"))?;
        let snapshot = app.call("notifications.snapshot", json!({}))?;
        judge_notification(&snapshot, sequence)?;
        let entry = snapshot["entries"]
            .as_array()
            .unwrap()
            .iter()
            .find(|entry| entry["sequence"].as_f64() == Some(sequence))
            .unwrap();
        product_ensure!(
            entry["severity"] == severity,
            "synthetic {kind} notification has severity {}, expected {severity}",
            entry["severity"]
        );
        ctx.evidence
            .put(&format!("notification{sequence}"), snapshot);
        if !ctx.ask(&format!(
            "Can you see the {severity} notification on the desktop or in the open hub?"
        ))? {
            return Err(Stop::unavailable(format!(
                "the operator cannot see the {severity} notification"
            )));
        }
        product_ensure!(
            ctx.ask(&format!(
                "Does the {severity} notification show the right glyph, tint and text?"
            ))?,
            "the operator judged the {severity} notification's appearance incorrect"
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn overlay_oracle_requires_native_visible_window() {
        let hidden = json!({"overlays": [{"objectName": "quickOverlayRecording", "visible": false, "nativeWindowCreated": true}]});
        assert!(matches!(
            judge_overlays(&hidden, &["quickOverlayRecording"]),
            Err(Stop::Infra(_))
        ));
        let visible = json!({"overlays": [{"objectName": "quickOverlayRecording", "visible": true, "nativeWindowCreated": true}]});
        judge_overlays(&visible, &["quickOverlayRecording"]).unwrap();
        assert!(matches!(
            judge_overlays(
                &visible,
                &["quickOverlayRecording", "quickOverlayCountdown"]
            ),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn notification_oracle_requires_new_active_toast_with_severity() {
        let snapshot = json!({"entries": [{"sequence": 12, "title": "Check", "severity": "caution"}], "activeToasts": []});
        assert!(matches!(
            judge_notification(&snapshot, 12.0),
            Err(Stop::Infra(_))
        ));
        let snapshot = json!({"entries": [{"sequence": 12, "title": "Check", "severity": "caution"}], "activeToasts": [{"sequence": 12, "title": "Check"}]});
        judge_notification(&snapshot, 12.0).unwrap();
    }
}
