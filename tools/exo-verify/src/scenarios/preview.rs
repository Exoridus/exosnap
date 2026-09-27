//! Live Preview liveness: the preview keeps consuming and rendering frames of
//! a changing source, on one monitor and across monitor boundaries.

use serde_json::{Value, json};
use std::time::{Duration, Instant};

use super::common::{self, Stimulus, StimulusOptions, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "preview.live-frames",
            revision: 1,
            title: "The Live Preview keeps presenting a changing source",
            class: ScenarioClass::Contract,
            contract: "with a capture source selected, the Live Preview keeps consuming and rendering new frames on any monitor topology, including a single SDR monitor",
            lane: Lane::Gpu,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop, Capability::Wgc],
            timeout: secs(120.0),
            run: live_frames,
        },
        Scenario {
            id: "preview.cross-monitor",
            revision: 1,
            title: "Moving the main window across monitors never freezes the preview",
            class: ScenarioClass::Hardware,
            contract: "moving the main window to another monitor and back never freezes the Live Preview while the pointer is still",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::MultiMonitor,
                Capability::Wgc,
            ],
            timeout: secs(180.0),
            run: cross_monitor,
        },
        Scenario {
            id: "preview.interactive-drag",
            revision: 1,
            title: "The preview keeps presenting through a real window drag across monitors",
            class: ScenarioClass::Hardware,
            contract: "the Live Preview keeps presenting during and after an operator's interactive drag of the main window across a monitor boundary, which enters the native modal move loop that programmatic moves never reach",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::MultiMonitor,
                Capability::Operator,
                Capability::Wgc,
            ],
            timeout: secs(600.0),
            run: interactive_drag,
        },
    ]
}

fn counters(sample: &Value) -> Step<(f64, f64)> {
    let consumed = sample["consumedFrames"]
        .as_f64()
        .ok_or_else(|| Stop::infra(format!("preview.snapshot has no consumedFrames: {sample}")))?;
    let rendered = sample["updateGate"]["renderPasses"]
        .as_f64()
        .ok_or_else(|| Stop::infra(format!("preview.snapshot has no renderPasses: {sample}")))?;
    Ok((consumed, rendered))
}

/// Selects the stimulus window as the capture source and waits for the
/// preview's first frame of it.
fn preview_stimulus(ctx: &mut Context, app: &mut App) -> Step<Stimulus> {
    let stimulus = Stimulus::start(ctx, StimulusOptions::default())?;
    common::select_window(app, &stimulus.title)?;
    app.client
        .poll("preview.snapshot", json!({}), secs(15.0), |preview| {
            preview["available"] == true && preview["frameReady"] == true
        })?
        .ok_or_else(|| {
            Stop::fail("the preview showed no frame of the selected window within 15 s")
        })?;
    Ok(stimulus)
}

/// The fewest new frames a live preview consumes and renders over `interval_s`:
/// half of what the source presents, and never more than half of what the
/// main window's display can show, because rendering is paced by that display.
fn required_advance(stimulus_rate: f64, display_hz: Option<f64>, interval_s: f64) -> f64 {
    let rate = display_hz
        .filter(|hz| hz.is_finite() && *hz > 0.0)
        .map_or(stimulus_rate, |hz| stimulus_rate.min(hz));
    0.5 * rate * interval_s
}

fn judge_liveness(before: &Value, after: &Value, interval_s: f64, required: f64) -> Step {
    let (consumed_before, rendered_before) = counters(before)?;
    let (consumed_after, rendered_after) = counters(after)?;
    infra_ensure!(
        required > 0.0,
        "no frame-rate expectation could be derived from the stimulus"
    );
    let consumed = consumed_after - consumed_before;
    let rendered = rendered_after - rendered_before;
    product_ensure!(
        consumed >= required,
        "the preview consumed {consumed} frames in {interval_s:.1} s of a changing source, expected at least {required:.0}"
    );
    product_ensure!(
        rendered >= required,
        "the preview rendered {rendered} passes in {interval_s:.1} s of a changing source, expected at least {required:.0}"
    );
    Ok(())
}

fn live_frames(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let mut stimulus = preview_stimulus(ctx, &mut app)?;
    let window = app.call("window.snapshot", json!({}))?;
    let display_hz = window["screen"]["refreshHz"].as_f64();
    let before = app.call("preview.snapshot", json!({}))?;
    let started = Instant::now();
    std::thread::sleep(secs(3.0));
    let after = app.call("preview.snapshot", json!({}))?;
    let interval = started.elapsed().as_secs_f64();
    let rate = stimulus.frame_rate()?;
    stimulus.stop();
    let required = required_advance(rate, display_hz, interval);
    ctx.evidence.put("previewBefore", before.clone());
    ctx.evidence.put("previewAfter", after.clone());
    ctx.evidence.put("stimulusRate", rate);
    ctx.evidence.put("mainWindowRefreshHz", json!(display_hz));
    ctx.evidence.put("requiredAdvance", required);
    judge_liveness(&before, &after, interval, required)
}

/// Judges the preview across one crossing from samples taken after it. Both
/// counters have to advance. A publish that no render pass followed across
/// every sample is the frozen-preview defect itself; no consumption and no
/// owed publish means the source delivered nothing, which judges nothing.
fn judge_crossing(samples: &[Value], crossing: &str) -> Step {
    infra_ensure!(
        samples.len() >= 2,
        "preview progress needs at least two observations"
    );
    let (consumed_start, rendered_start) = counters(&samples[0])?;
    let (consumed_end, rendered_end) = counters(samples.last().unwrap())?;
    let consumed = consumed_end > consumed_start;
    let rendered = rendered_end > rendered_start;
    if consumed && rendered {
        return Ok(());
    }
    if !rendered
        && samples
            .iter()
            .all(|sample| sample["updateGate"]["owed"] == true)
    {
        return Err(Stop::fail(format!(
            "a published preview frame stayed unrendered after {crossing}"
        )));
    }
    if consumed {
        return Err(Stop::fail(format!(
            "the preview consumed new frames but rendered none after {crossing}"
        )));
    }
    Err(Stop::infra(format!(
        "the preview consumed no frame after {crossing} and no persistent publish debt was observed"
    )))
}

fn sample_after_crossing(app: &mut App, crossing: &str, evidence: &mut Vec<Value>) -> Step {
    let settled = app.call("preview.snapshot", json!({}))?;
    std::thread::sleep(secs(2.0));
    let later = app.call("preview.snapshot", json!({}))?;
    let samples = [settled, later];
    evidence.push(json!({ "crossing": crossing, "samples": samples }));
    judge_crossing(&samples, crossing)
}

fn cross_monitor(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    let environment = app.call("environment.snapshot", json!({}))?;
    let names: Vec<String> = environment["displays"]["screens"]
        .as_array()
        .ok_or_else(|| Stop::infra("environment.snapshot has no screens array"))?
        .iter()
        .filter_map(|screen| screen["name"].as_str().map(str::to_string))
        .collect();
    let mut stimulus = preview_stimulus(ctx, &mut app)?;
    let home = common::main_window_screen(&app.call("window.snapshot", json!({}))?)?;
    let others: Vec<&String> = names.iter().filter(|name| **name != home).collect();
    if others.is_empty() {
        stimulus.stop();
        return Err(Stop::unavailable(
            "the product reports no screen besides the main window's own",
        ));
    }
    let mut evidence = Vec::new();
    let outcome = (|| -> Step {
        for other in others {
            common::move_to_screen(&mut app, other, secs(15.0))?;
            sample_after_crossing(&mut app, &format!("moving to {other}"), &mut evidence)?;
            common::move_to_screen(&mut app, &home, secs(15.0))?;
            sample_after_crossing(&mut app, &format!("moving back to {home}"), &mut evidence)?;
        }
        Ok(())
    })();
    stimulus.stop();
    ctx.evidence.put("crossings", json!(evidence));
    outcome
}

fn interactive_drag(ctx: &mut Context) -> Step {
    ctx.require(Capability::Operator)?;
    let mut app = ctx.launch(&[])?;
    let mut stimulus = preview_stimulus(ctx, &mut app)?;
    common::drain_events(&mut app, "window.screenChanged")?;
    let before = app.call("preview.snapshot", json!({}))?;
    let dragged = ctx.ask(
        "Drag the ExoSnap main window by its title bar onto another monitor and back, release it, keep the pointer still, then answer yes.",
    );
    let outcome = (|| -> Step {
        if !dragged? {
            return Err(Stop::unavailable(
                "the operator did not perform the interactive drag",
            ));
        }
        let after_drag = app.call("preview.snapshot", json!({}))?;
        let mut crossings = 0;
        while app
            .client
            .wait_event("window.screenChanged", &json!({}), Duration::ZERO)?
            .is_some()
        {
            crossings += 1;
        }
        ctx.evidence.put("screenChangedEvents", crossings);
        ctx.evidence.put("previewBeforeDrag", before.clone());
        ctx.evidence.put("previewAfterDrag", after_drag.clone());
        infra_ensure!(
            crossings > 0,
            "the product reported no screen change, so the drag never crossed a monitor boundary"
        );
        let (_, rendered_before) = counters(&before)?;
        let (_, rendered_after) = counters(&after_drag)?;
        product_ensure!(
            rendered_after > rendered_before,
            "the preview rendered nothing while the window was dragged across monitors"
        );
        std::thread::sleep(secs(2.0));
        let settled = app.call("preview.snapshot", json!({}))?;
        ctx.evidence.put("previewSettled", settled.clone());
        judge_crossing(&[after_drag, settled], "the interactive drag")
    })();
    stimulus.stop();
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(consumed: u32, rendered: u32, owed: bool) -> Value {
        json!({"consumedFrames": consumed, "updateGate": {"renderPasses": rendered, "owed": owed}})
    }

    #[test]
    fn liveness_expectation_is_capped_by_the_display_that_renders_it() {
        assert_eq!(required_advance(60.0, Some(60.0), 3.0), 90.0);
        assert_eq!(required_advance(144.0, Some(60.0), 3.0), 90.0);
        assert_eq!(required_advance(60.0, Some(144.0), 3.0), 90.0);
        assert_eq!(required_advance(60.0, None, 2.0), 60.0);
        assert_eq!(required_advance(60.0, Some(0.0), 2.0), 60.0);
    }

    #[test]
    fn liveness_requires_both_counters_to_keep_pace() {
        judge_liveness(&sample(10, 10, false), &sample(110, 100, false), 3.0, 90.0).unwrap();
        assert!(matches!(
            judge_liveness(&sample(10, 10, false), &sample(50, 100, false), 3.0, 90.0),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_liveness(&sample(10, 10, false), &sample(110, 11, true), 3.0, 90.0),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_liveness(&json!({}), &sample(110, 100, false), 3.0, 90.0),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn crossing_requires_consumption_and_rendering() {
        judge_crossing(&[sample(10, 5, false), sample(11, 6, true)], "x").unwrap();
        assert!(matches!(
            judge_crossing(&[sample(10, 5, true), sample(10, 5, true)], "x"),
            Err(Stop::Fail(_))
        ));
        assert!(matches!(
            judge_crossing(&[sample(10, 5, false), sample(20, 5, false)], "x"),
            Err(Stop::Fail(_))
        ));
    }

    #[test]
    fn crossing_without_published_work_is_infrastructure() {
        assert!(matches!(
            judge_crossing(&[sample(10, 5, false), sample(10, 5, false)], "x"),
            Err(Stop::Infra(_))
        ));
        assert!(matches!(
            judge_crossing(
                &[
                    json!({"consumedFrames": 10, "updateGate": {"owed": true}}),
                    sample(11, 6, false)
                ],
                "x"
            ),
            Err(Stop::Infra(_))
        ));
    }
}
