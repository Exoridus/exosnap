//! Application lifecycle and the control surface itself, against official bytes.

use serde_json::{Value, json};
use std::process::Command;
use std::time::{Duration, Instant};

use super::common::secs;
use super::dist::{STATUS_DLL_NOT_FOUND, suppress_error_dialogs};
use crate::capability::Capability;
use crate::context::Context;
use crate::control::{Client, PROTOCOL_V1};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "app.smoke-start",
            revision: 1,
            title: "The product's own smoke start exits cleanly",
            claim: "exosnap.exe --smoke-test initialises the full application and exits with success",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(90.0),
            run: smoke_start,
        },
        Scenario {
            id: "app.control-endpoint-lifecycle",
            revision: 1,
            title: "The control endpoint exists only when armed and only while the process lives",
            claim: "a normal launch exposes no control pipe; an armed launch handshakes with the expected identity; the pipe disappears with the process",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: control_endpoint_lifecycle,
        },
        Scenario {
            id: "app.protocol-compat",
            revision: 2,
            title: "Protocol 1 and protocol 2 work in sequence on the same endpoint",
            claim: "a protocol-1 client gets protocol-1 envelopes without protocol-2 fields and cannot reach protocol-2 commands; a subsequent protocol-2 client retains its fields",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Recommended,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(90.0),
            run: protocol_compat,
        },
        Scenario {
            id: "app.navigation-surfaces",
            revision: 1,
            title: "Every page and popup is reachable and settles",
            claim: "each page navigates and settles idempotently, Edit is not a navigation target, and the source picker, notification hub and settings reveal behave as specified",
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
            claim: "a close request ends the process within 20 s although a connected client has stopped reading buffered events",
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
            claim: "with in-depth diagnostics opted in but no elevation, present diagnostics report requiresElevation and claim no data",
            lane: Lane::Gpu,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(90.0),
            run: present_unelevated,
        },
    ]
}

fn smoke_start(ctx: &mut Context) -> Step {
    suppress_error_dialogs();
    let product = ctx.product()?;
    let config = ctx.scenario_dir.join("config");
    std::fs::create_dir_all(&config)?;
    let mut child = ctx.spawn(
        Command::new(&product.exe)
            .arg("--smoke-test")
            .current_dir(&product.root)
            .env("EXOSNAP_CONFIG_DIR", &config)
            .env("EXOSNAP_OUTPUT_DIR", ctx.scenario_dir.join("output")),
    )?;
    let status = crate::tools::wait(&mut child, Duration::from_secs(60))
        .map_err(|_| Stop::fail("--smoke-test did not exit within 60 s"))?;
    match status.code() {
        Some(0) => Ok(()),
        Some(STATUS_DLL_NOT_FOUND) => Err(Stop::fail(
            "a required DLL is missing (STATUS_DLL_NOT_FOUND)",
        )),
        other => Err(Stop::fail(format!("--smoke-test exited with {other:?}"))),
    }
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
    app.close()?;
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

fn protocol_compat(ctx: &mut Context) -> Step {
    let app = ctx.launch(&[])?;
    drop(app.client);
    let mut v1 = Client::connect_protocol("LiveVerify", &app.run_id, secs(15.0), PROTOCOL_V1)
        .map_err(|e| {
            Stop::fail(format!(
                "a protocol-1 client could not connect after protocol 2: {e:#}"
            ))
        })?;
    v1.request("record.snapshot", json!({}), secs(10.0))?
        .map_err(|r| Stop::fail(format!("protocol 1 refused record.snapshot: {r}")))?;
    let envelope = v1.last_response.clone();
    for field in ["stateRevision", "settled", "state"] {
        product_ensure!(
            envelope.get(field).is_none(),
            "a protocol-1 response carries the protocol-2 field '{field}'"
        );
    }
    match v1.request("ui.getState", json!({}), secs(10.0))? {
        Ok(_) => {
            return Err(Stop::fail(
                "protocol 1 answered the protocol-2 command ui.getState",
            ));
        }
        Err(r) => product_ensure!(
            r.code == "unknown_command",
            "protocol 1 refused ui.getState with {} instead of unknown_command",
            r.code
        ),
    }
    drop(v1);
    let mut v2 = Client::connect("LiveVerify", &app.run_id, secs(15.0))?;
    v2.call("record.snapshot", json!({}))?;
    product_ensure!(
        v2.last_response.get("stateRevision").is_some(),
        "protocol 2 lost its stateRevision field"
    );
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
