//! Application lifecycle and the control surface itself, against official bytes.

use serde_json::{Value, json};
use std::process::Command;
use std::time::{Duration, Instant};

use super::common::{self, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "app.control-endpoint-lifecycle",
            revision: 1,
            title: "The control endpoint exists only when armed and only while the process lives",
            class: ScenarioClass::Contract,
            contract: "a normal launch exposes no control pipe; an armed launch handshakes with the expected identity; the pipe disappears with the process",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: control_endpoint_lifecycle,
        },
        Scenario {
            id: "app.native-chrome",
            revision: 1,
            title: "The main window keeps Windows' native gestures without a native caption",
            class: ScenarioClass::Contract,
            contract: "the main window carries the window styles Windows derives edge resize, Snap, minimize, maximize and the system menu from, and has no native caption, border or child window, before and after navigation and a move to another screen",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::Windows, Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: native_chrome,
        },
        Scenario {
            id: "app.navigation-surfaces",
            revision: 1,
            title: "Every page and popup is reachable and settles",
            class: ScenarioClass::Contract,
            contract: "each page navigates and settles idempotently, Edit is not a navigation target, and the source picker, notification hub and settings reveal behave as specified",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: navigation_surfaces,
        },
        Scenario {
            id: "app.exit-with-unread-events",
            revision: 1,
            title: "Unread control events never hold the application open",
            class: ScenarioClass::Regression,
            contract: "a close request ends the process within 20 s although a connected client has stopped reading buffered events",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: exit_with_unread_events,
        },
        Scenario {
            id: "diagnostics.present-unelevated",
            revision: 1,
            title: "Unelevated present diagnostics explain themselves and open nothing",
            class: ScenarioClass::Contract,
            contract: "with in-depth diagnostics opted in but no elevation, present diagnostics report requiresElevation and claim no data",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(90.0),
            run: present_unelevated,
        },
    ]
}

#[cfg(windows)]
fn live_verify_pipes() -> Vec<String> {
    crate::win::named_pipes("ExoSnap.LiveVerify.")
}

#[cfg(not(windows))]
fn live_verify_pipes() -> Vec<String> {
    vec![]
}

fn control_endpoint_lifecycle(ctx: &mut Context) -> Step {
    let before = live_verify_pipes();
    infra_ensure!(
        before.is_empty(),
        "another ExoSnap control endpoint already exists: {before:?}"
    );
    let product = ctx.product()?;
    let config = ctx.scenario_dir.join("plain-config");
    std::fs::create_dir_all(&config)?;
    let mut plain = ctx.spawn(
        Command::new(&product.exe)
            .current_dir(&product.root)
            .env("EXOSNAP_CONFIG_DIR", &config),
    )?;
    // A visible main window means startup, where an endpoint would be created, is over.
    let deadline = Instant::now() + secs(45.0);
    loop {
        infra_ensure!(
            plain.try_wait()?.is_none(),
            "the plain launch exited on its own"
        );
        #[cfg(windows)]
        if !crate::win::process_windows(plain.id()).is_empty() {
            break;
        }
        infra_ensure!(
            Instant::now() < deadline,
            "the plain launch showed no window within 45 s"
        );
        std::thread::sleep(Duration::from_millis(200));
    }
    let leaked = live_verify_pipes();
    let _ = plain.kill();
    let _ = plain.wait();
    product_ensure!(
        leaked.is_empty(),
        "a launch without --live-verify-control created a control endpoint: {leaked:?}"
    );

    let app = ctx.launch(&[])?;
    let id = app.identity().clone();
    product_ensure!(
        id["runId"] == app.run_id.as_str() || id.get("runId").is_none(),
        "the endpoint echoed another run id"
    );
    let pid = id["pid"].as_u64().unwrap_or(0) as u32;
    product_ensure!(
        pid == app.child.id(),
        "the endpoint reports pid {pid}, the launched process is {}",
        app.child.id()
    );
    let pipe = format!("ExoSnap.LiveVerify.{}", app.run_id);
    product_ensure!(
        live_verify_pipes().contains(&pipe),
        "the armed endpoint {pipe} is not listed"
    );
    app.kill_for_cleanup()?;
    let deadline = Instant::now() + secs(10.0);
    while live_verify_pipes().contains(&pipe) {
        product_ensure!(
            Instant::now() < deadline,
            "the endpoint {pipe} outlived its process"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
    Ok(())
}

const PAGES: [&str; 5] = ["record", "settings", "diagnostics", "logs", "about"];

fn navigation_surfaces(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    for page in PAGES {
        for attempt in 0..2 {
            let result = app
                .client
                .request("ui.navigate", json!({ "page": page }), secs(15.0))?
                .map_err(|r| Stop::fail(format!("ui.navigate {page} refused: {r}")))?;
            product_ensure!(
                result["page"] == page,
                "navigating to {page} (attempt {}) landed on {}",
                attempt + 1,
                result["page"]
            );
            product_ensure!(
                app.client.last_response["settled"] == true,
                "ui.navigate {page} did not settle in its response"
            );
        }
    }
    match app
        .client
        .request("ui.navigate", json!({ "page": "edit" }), secs(10.0))?
    {
        Ok(_) => {
            return Err(Stop::fail(
                "Edit was accepted as a navigation destination; it is an overlay",
            ));
        }
        Err(r) => product_ensure!(
            r.code == "invalid_params",
            "navigating to edit was refused with {} instead of invalid_params",
            r.code
        ),
    }
    app.call("ui.navigate", json!({ "page": "record" }))?;
    for (open, close, field) in [
        ("sourcePicker.open", "sourcePicker.close", "sourcePicker"),
        (
            "notificationHub.open",
            "notificationHub.close",
            "notificationHub",
        ),
    ] {
        for _ in 0..2 {
            app.call(open, json!({}))?;
            let state = app.call("ui.getState", json!({}))?;
            product_ensure!(
                is_open(&state, field),
                "{open} did not leave {field} open: {}",
                state[field]
            );
            app.call(close, json!({}))?;
            let state = app.call("ui.getState", json!({}))?;
            product_ensure!(!is_open(&state, field), "{close} did not close {field}");
        }
    }
    app.call("ui.navigate", json!({ "page": "settings" }))?;
    match app.client.request(
        "ui.reveal",
        json!({ "surface": "settings", "target": "no-such-section" }),
        secs(10.0),
    )? {
        Ok(_) => {
            return Err(Stop::fail(
                "revealing an unknown settings section succeeded",
            ));
        }
        Err(r) => product_ensure!(
            r.code == "invalid_params",
            "an unknown reveal target was refused with {}",
            r.code
        ),
    }
    Ok(())
}

/// The window styles Windows derives edge resize, Snap, minimize, maximize and
/// the system menu from. A frameless window keeps the gestures only while it
/// keeps these bits.
const NATIVE_GESTURE_STYLES: [(u32, &str); 4] = [
    (0x0004_0000, "WS_THICKFRAME (edge resize and Snap)"),
    (0x0002_0000, "WS_MINIMIZEBOX (minimize)"),
    (0x0001_0000, "WS_MAXIMIZEBOX (maximize and Snap)"),
    (0x0008_0000, "WS_SYSMENU (system menu)"),
];

/// Child HWNDs intercept WM_NCHITTEST before the frameless shell sees it, so
/// the main window must have none.
const EXPECTED_CHILD_HWNDS: i64 = 0;

fn judge_native_chrome(window: &Value, when: &str) -> Step {
    let native = &window["native"];
    infra_ensure!(
        native["valid"] == true,
        "window.snapshot {when} carries no valid native facts: {native}"
    );
    let style_text = native["style"]
        .as_str()
        .ok_or_else(|| Stop::infra(format!("window.snapshot {when} has no native style")))?;
    let style = u32::from_str_radix(style_text.trim_start_matches("0x"), 16)
        .map_err(|_| Stop::infra(format!("native style {style_text:?} is not hexadecimal")))?;
    for (bit, name) in NATIVE_GESTURE_STYLES {
        product_ensure!(
            style & bit == bit,
            "{when}: the main window style {style_text} lacks {name}"
        );
    }
    product_ensure!(
        native["nativeTitlebar"] == false,
        "{when}: the main window draws a native title bar"
    );
    for side in ["Left", "Top", "Right", "Bottom"] {
        let inset = native[format!("nonClientInset{side}")]
            .as_i64()
            .ok_or_else(|| Stop::infra(format!("window.snapshot has no nonClientInset{side}")))?;
        product_ensure!(
            inset == 0,
            "{when}: the main window has a {inset} px native {side} border"
        );
    }
    let children = native["childHwnds"]
        .as_i64()
        .ok_or_else(|| Stop::infra("window.snapshot has no childHwnds count"))?;
    product_ensure!(
        children == EXPECTED_CHILD_HWNDS,
        "{when}: the main window has {children} child HWNDs, expected {EXPECTED_CHILD_HWNDS}"
    );
    Ok(())
}

fn observe_native_chrome(app: &mut App, when: &str, seen: &mut Vec<Value>) -> Step {
    let window = app.call("window.snapshot", json!({}))?;
    seen.push(json!({ "when": when, "window": window.clone() }));
    judge_native_chrome(&window, when)
}

fn native_chrome(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let mut seen = Vec::new();
    let shown = app
        .client
        .poll("window.snapshot", json!({}), secs(30.0), |window| {
            window["visible"] == true && window["native"]["valid"] == true
        })?
        .ok_or_else(|| {
            Stop::fail("the main window was not shown with a native handle within 30 s")
        })?;
    seen.push(json!({ "when": "shown", "window": shown.clone() }));
    let outcome = (|| -> Step {
        judge_native_chrome(&shown, "when first shown")?;
        for page in PAGES {
            app.client
                .request("ui.navigate", json!({ "page": page }), secs(15.0))?
                .map_err(|r| Stop::fail(format!("ui.navigate {page} was refused: {r}")))?;
            observe_native_chrome(&mut app, &format!("on the {page} page"), &mut seen)?;
        }
        let home = common::main_window_screen(&shown)?;
        let environment = app.call("environment.snapshot", json!({}))?;
        let other = environment["displays"]["screens"]
            .as_array()
            .and_then(|screens| {
                screens
                    .iter()
                    .filter_map(|screen| screen["name"].as_str())
                    .find(|name| *name != home)
            })
            .map(str::to_string);
        if let Some(other) = other {
            common::move_to_screen(&mut app, &other, secs(15.0))?;
            observe_native_chrome(&mut app, &format!("after moving to {other}"), &mut seen)?;
            common::move_to_screen(&mut app, &home, secs(15.0))?;
            observe_native_chrome(&mut app, &format!("after moving back to {home}"), &mut seen)?;
        }
        Ok(())
    })();
    ctx.evidence.put("windowSnapshots", json!(seen));
    outcome
}

fn is_open(state: &Value, field: &str) -> bool {
    match &state[field] {
        Value::Bool(b) => *b,
        Value::String(s) => s == "open",
        Value::Object(o) => o.get("open").and_then(Value::as_bool).unwrap_or(false),
        _ => false,
    }
}

fn exit_with_unread_events(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    // Closing must end the process, not hide it in the tray.
    super::common::configure(&mut app, &[("app.minimizeToTray", json!(false))])?;
    for page in ["diagnostics", "settings", "record", "logs"] {
        app.call("ui.navigate", json!({ "page": page }))?;
    }
    app.client
        .request("diagnostics.run", json!({}), secs(10.0))?
        .ok();
    app.client.stop_reading();
    // More state changes after the client stopped reading: their events queue up.
    let pid = app.child.id();
    #[cfg(windows)]
    {
        let closed = crate::win::close_windows(pid);
        infra_ensure!(closed > 0, "the product has no visible window to close");
    }
    match crate::tools::wait(&mut app.child, Duration::from_secs(20)) {
        Ok(_) => Ok(()),
        Err(_) => Err(Stop::fail(
            "the process was still running 20 s after its window was closed while a client held unread events",
        )),
    }
}

fn present_unelevated(ctx: &mut Context) -> Step {
    if ctx.has(Capability::Admin) {
        return Err(Stop::unavailable(
            "this runner is elevated; the unelevated contract cannot be observed from it",
        ));
    }
    let mut app = ctx.launch(&[])?;
    app.call("diagnostics.setInDepth", json!({ "enabled": true }))?;
    let env = app.call("environment.snapshot", json!({}))?;
    let present = &env["present"];
    ctx.evidence.put("present", present.clone());
    product_ensure!(
        present["optIn"] == true,
        "the opt-in did not take: {present}"
    );
    product_ensure!(
        present["available"] == false,
        "present data is claimed without elevation: {present}"
    );
    product_ensure!(
        present["availability"] == "requiresElevation",
        "availability is {} instead of requiresElevation",
        present["availability"]
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(style: &str, top_inset: i64, children: i64) -> Value {
        json!({"native": {
            "valid": true,
            "style": style,
            "nativeTitlebar": top_inset > 0,
            "nonClientInsetLeft": 0,
            "nonClientInsetTop": top_inset,
            "nonClientInsetRight": 0,
            "nonClientInsetBottom": 0,
            "childHwnds": children
        }})
    }

    #[test]
    fn native_chrome_requires_every_gesture_style_and_no_native_frame() {
        // WS_OVERLAPPEDWINDOW | WS_VISIBLE carries all four gesture bits.
        judge_native_chrome(&window("0x16cf0000", 0, 0), "now").unwrap();
        for (bit, _) in NATIVE_GESTURE_STYLES {
            let style = format!("0x{:08x}", 0x16cf_0000u32 & !bit);
            assert!(matches!(
                judge_native_chrome(&window(&style, 0, 0), "now"),
                Err(Stop::Fail(_))
            ));
        }
        assert!(matches!(
            judge_native_chrome(&window("0x16cf0000", 31, 0), "now"),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_native_chrome(&window("0x16cf0000", 0, 1), "now"),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_native_chrome(&json!({"native": {"valid": false}}), "now"),
            Err(Stop::Infra(_))
        ));
    }
}
