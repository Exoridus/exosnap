//! Capture overlays: absent from the recorded file, and operable or
//! click-through exactly as specified.

use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::time::{Duration, Instant};

use super::common::{self, Stimulus, StimulusOptions, secs};
use crate::capability::Capability;
use crate::context::{App, Context};
use crate::media::{self, LumaFrame};
use crate::pattern::Rect;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "overlay.not-recorded",
            revision: 1,
            title: "Capture overlays never appear in a recording",
            class: ScenarioClass::Contract,
            contract: "the recording pill (carrying the diagnostics tokens), quick controls and notification toast shown during a display recording are absent from the recorded file",
            lane: Lane::Gpu,
            also: &[Lane::Preflight],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::DxgiDuplication,
                Capability::Nvenc,
                Capability::Ffprobe,
                Capability::Ffmpeg,
            ],
            timeout: secs(240.0),
            run: not_recorded,
        },
        Scenario {
            id: "overlay.operable-hit-test",
            revision: 1,
            title: "Toast and quick controls take clicks; the other overlays let them through",
            class: ScenarioClass::Contract,
            contract: "during a recording the notification toast and quick controls receive clicks at their centre, while the recording pill and diagnostics overlay pass clicks to the window beneath",
            lane: Lane::Gpu,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[
                Capability::InteractiveDesktop,
                Capability::Wgc,
                Capability::Nvenc,
            ],
            timeout: secs(120.0),
            run: operable_hit_test,
        },
    ]
}

// The recording pill's window carries the diagnostics tokens too now
// (OverlayDiagnostics.qml was absorbed into OverlayRecording.qml); there is
// no separate "quickOverlayDiagnostics" window left to name.
const PILL: &str = "quickOverlayRecording";
const QUICK_CONTROLS: &str = "quickOverlayQuickControls";
const TOAST: &str = "quickOverlayNotificationToast";

/// Overlays a recording shows. The countdown overlay is not among them: it is
/// gone before capture starts, and the control channel has no setting that
/// schedules one.
const RECORDING_OVERLAYS: [&str; 3] = [PILL, QUICK_CONTROLS, TOAST];

const OPERABLE: [&str; 2] = [TOAST, QUICK_CONTROLS];

/// Mean absolute luma difference (0-255) above which an overlay's rectangle in
/// the recording no longer shows what was beneath it. Encoding noise of a still
/// source stays in low single digits; an overlay surface differs by tens.
const MAX_MEAN_LUMA_DIFFERENCE: f64 = 8.0;

/// Screen rectangle in virtual-screen physical pixels: left, top, right, bottom.
type ScreenRect = [i32; 4];

fn native_rect(overlay: &Value) -> Option<ScreenRect> {
    let native = &overlay["native"];
    if native["valid"] != true {
        return None;
    }
    let x = native["x"].as_i64()? as i32;
    let y = native["y"].as_i64()? as i32;
    let w = native["width"].as_i64()? as i32;
    let h = native["height"].as_i64()? as i32;
    (w > 0 && h > 0).then_some([x, y, x + w, y + h])
}

fn visible_overlays(snapshot: &Value) -> Step<BTreeMap<String, (ScreenRect, Value)>> {
    let overlays = snapshot["overlays"]
        .as_array()
        .ok_or_else(|| Stop::infra("overlay.snapshot has no overlays array"))?;
    Ok(overlays
        .iter()
        .filter(|overlay| overlay["visible"] == true && overlay["nativeWindowCreated"] == true)
        .filter_map(|overlay| {
            let name = overlay["objectName"].as_str()?.to_string();
            Some((name, (native_rect(overlay)?, overlay.clone())))
        })
        .collect())
}

fn raise_toast(app: &mut App) -> Step {
    app.client
        .request(
            "notification.raise",
            json!({
                "type": "saved",
                "title": "Overlay check",
                "body": "Synthetic toast for overlay verification"
            }),
            secs(15.0),
        )?
        .map_err(|r| Stop::infra(format!("notification.raise was refused: {r}")))?;
    Ok(())
}

fn configure_overlays(app: &mut App, shown: bool) -> Step {
    let settings: Vec<(&str, Value)> = vec![
        ("app.showRecordingOverlay", json!(shown)),
        ("app.showQuickControls", json!(shown)),
        ("app.showDiagnosticsOverlay", json!(shown)),
        ("app.showNotifications", json!(shown)),
    ];
    let changed = common::configure(app, &settings)?;
    infra_ensure!(
        changed.is_empty(),
        "the product did not accept the overlay settings: {changed:?}"
    );
    Ok(())
}

// ---------------------------------------------------------------------------
// overlay.not-recorded
// ---------------------------------------------------------------------------

/// Maps a screen rectangle onto a frame of the recorded monitor, clipped to the
/// frame. `None` when the rectangle lies outside the monitor.
fn frame_rect(screen: ScreenRect, monitor: ScreenRect, width: u32, height: u32) -> Option<Rect> {
    let (mw, mh) = (
        (monitor[2] - monitor[0]) as f64,
        (monitor[3] - monitor[1]) as f64,
    );
    if mw <= 0.0 || mh <= 0.0 {
        return None;
    }
    let sx = width as f64 / mw;
    let sy = height as f64 / mh;
    let clip = |value: f64, limit: u32| value.clamp(0.0, limit as f64);
    let x0 = clip(((screen[0] - monitor[0]) as f64 * sx).floor(), width);
    let y0 = clip(((screen[1] - monitor[1]) as f64 * sy).floor(), height);
    let x1 = clip(((screen[2] - monitor[0]) as f64 * sx).ceil(), width);
    let y1 = clip(((screen[3] - monitor[1]) as f64 * sy).ceil(), height);
    (x1 > x0 && y1 > y0).then_some(Rect {
        x: x0 as u32,
        y: y0 as u32,
        w: (x1 - x0) as u32,
        h: (y1 - y0) as u32,
    })
}

fn mean_abs_difference(a: &LumaFrame, b: &LumaFrame, rect: Rect) -> f64 {
    let mut sum = 0u64;
    let mut count = 0u64;
    for y in rect.y..(rect.y + rect.h).min(a.height).min(b.height) {
        for x in rect.x..(rect.x + rect.w).min(a.width).min(b.width) {
            let pa = a.data[(y * a.width + x) as usize];
            let pb = b.data[(y * b.width + x) as usize];
            sum += pa.abs_diff(pb) as u64;
            count += 1;
        }
    }
    if count == 0 {
        0.0
    } else {
        sum as f64 / count as f64
    }
}

/// When each overlay was seen on screen, in seconds since the recording
/// started, and where.
#[derive(Default)]
struct Sighting {
    first: f64,
    last: f64,
    rect: ScreenRect,
    capture_excluded: Value,
}

fn not_recorded(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("audio.systemEnabled", json!(false)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
            ("video.captureCursor", json!(false)),
        ],
    )?;
    configure_overlays(&mut app, false)?;
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            fullscreen: true,
            still: true,
            ..Default::default()
        },
    )?;
    let monitor = stimulus.rect;
    let outcome = (|| -> Step {
        common::select_display(&mut app, &stimulus.monitor)?;
        let (_, reference_file, _) = common::record_for(&mut app, 3.0)?;

        configure_overlays(&mut app, true)?;
        let started = common::start_recording(&mut app)?;
        raise_toast(&mut app)?;
        let mut sightings: BTreeMap<String, Sighting> = BTreeMap::new();
        while started.elapsed() < secs(5.0) {
            let snapshot = app.call("overlay.snapshot", json!({}))?;
            let at = started.elapsed().as_secs_f64();
            for (name, (rect, overlay)) in visible_overlays(&snapshot)? {
                let sighting = sightings.entry(name).or_insert_with(|| Sighting {
                    first: at,
                    rect,
                    capture_excluded: overlay["native"]["captureExcluded"].clone(),
                    ..Default::default()
                });
                sighting.last = at;
            }
            std::thread::sleep(Duration::from_millis(250));
        }
        let result = common::stop_recording(&mut app)?;
        let file = common::output_path(&result)?;

        ctx.evidence.put(
            "overlays",
            json!(
                sightings
                    .iter()
                    .map(|(name, s)| (
                        name.clone(),
                        json!({
                            "firstSeconds": s.first,
                            "lastSeconds": s.last,
                            "rect": s.rect,
                            "captureExcluded": s.capture_excluded
                        })
                    ))
                    .collect::<serde_json::Map<_, _>>()
            ),
        );
        ctx.evidence.put("monitorRect", json!(monitor));
        for name in RECORDING_OVERLAYS {
            infra_ensure!(
                sightings.contains_key(name),
                "{name} was never visible during the recording, so its absence proves nothing"
            );
        }

        const WIDTH: u32 = 640;
        let reference_frames = media::luma_frames(&reference_file, WIDTH)?;
        infra_ensure!(
            !reference_frames.is_empty(),
            "the reference recording decoded no frames"
        );
        let reference = &reference_frames[reference_frames.len() / 2];
        let frames = media::luma_frames(&file, WIDTH)?;
        let first_pts = frames.first().map(|f| f.pts).unwrap_or(0.0);
        let mut differences = serde_json::Map::new();
        for (name, sighting) in &sightings {
            let Some(rect) = frame_rect(sighting.rect, monitor, reference.width, reference.height)
            else {
                return Err(Stop::infra(format!(
                    "{name} was shown outside the recorded display"
                )));
            };
            // Half a second of margin on each side absorbs the offset between
            // the wall clock the sightings used and the file's timeline.
            let window = (sighting.first + 0.5)..(sighting.last - 0.25);
            let judged: Vec<&LumaFrame> = frames
                .iter()
                .filter(|frame| window.contains(&(frame.pts - first_pts)))
                .collect();
            infra_ensure!(
                !judged.is_empty(),
                "no recorded frame falls inside {name}'s visible interval"
            );
            let worst = judged
                .iter()
                .map(|frame| mean_abs_difference(frame, reference, rect))
                .fold(0.0, f64::max);
            differences.insert(name.clone(), json!(worst));
            product_ensure!(
                worst <= MAX_MEAN_LUMA_DIFFERENCE,
                "{name} is visible in the recording: its area differs from the unobstructed source by {worst:.1} luma levels"
            );
        }
        ctx.evidence.put("meanLumaDifference", differences);
        Ok(())
    })();
    stimulus.stop();
    outcome
}

// ---------------------------------------------------------------------------
// overlay.operable-hit-test
// ---------------------------------------------------------------------------

fn contains(rect: ScreenRect, (x, y): (i32, i32)) -> bool {
    x >= rect[0] && x < rect[2] && y >= rect[1] && y < rect[3]
}

/// A point inside `rect` that no other overlay covers: the centre when it is
/// free, else the first free quarter point.
fn probe_point(rect: ScreenRect, others: &[ScreenRect]) -> Option<(i32, i32)> {
    let (w, h) = (rect[2] - rect[0], rect[3] - rect[1]);
    [
        (2, 2),
        (1, 1),
        (3, 1),
        (1, 3),
        (3, 3),
        (1, 2),
        (3, 2),
        (2, 1),
        (2, 3),
    ]
    .into_iter()
    .map(|(fx, fy)| (rect[0] + w * fx / 4, rect[1] + h * fy / 4))
    .find(|point| !others.iter().any(|other| contains(*other, *point)))
}

/// The top-level window that receives a click at a physical screen point, and
/// its process.
#[cfg(windows)]
fn window_at((x, y): (i32, i32)) -> (u64, u32) {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::HiDpi::{
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        GA_ROOT, GetAncestor, GetWindowThreadProcessId, WindowFromPoint,
    };
    unsafe {
        // Overlay geometry is in physical pixels; an unaware thread would have
        // its point scaled by the system DPI before the hit test.
        let previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        let hit = WindowFromPoint(POINT { x, y });
        if !previous.is_invalid() {
            SetThreadDpiAwarenessContext(previous);
        }
        let root = GetAncestor(hit, GA_ROOT);
        let mut pid = 0u32;
        GetWindowThreadProcessId(root, Some(&mut pid));
        (root.0 as usize as u64, pid)
    }
}

#[cfg(not(windows))]
fn window_at(_: (i32, i32)) -> (u64, u32) {
    (0, 0)
}

fn operable_hit_test(ctx: &mut Context) -> Step {
    let mut app = ctx.launch(&[])?;
    common::configure_exact(
        &mut app,
        &[
            ("audio.systemEnabled", json!(false)),
            ("audio.appEnabled", json!(false)),
            ("audio.microphoneEnabled", json!(false)),
        ],
    )?;
    configure_overlays(&mut app, true)?;
    let mut stimulus = Stimulus::start(
        ctx,
        StimulusOptions {
            fullscreen: true,
            ..Default::default()
        },
    )?;
    let product_pid = app.child.id();
    let outcome = (|| -> Step {
        common::select_window(&mut app, &stimulus.title)?;
        common::start_recording(&mut app)?;
        raise_toast(&mut app)?;
        let deadline = Instant::now() + secs(10.0);
        let visible = loop {
            let snapshot = app.call("overlay.snapshot", json!({}))?;
            let visible = visible_overlays(&snapshot)?;
            if RECORDING_OVERLAYS
                .iter()
                .all(|name| visible.contains_key(*name))
            {
                break visible;
            }
            infra_ensure!(
                Instant::now() < deadline,
                "not every recording overlay became visible within 10 s: {:?}",
                visible.keys().collect::<Vec<_>>()
            );
            std::thread::sleep(Duration::from_millis(200));
        };
        let mut observations = serde_json::Map::new();
        let mut verdicts = Vec::new();
        for name in RECORDING_OVERLAYS {
            let (rect, overlay) = &visible[name];
            let others: Vec<ScreenRect> = visible
                .iter()
                .filter(|(other, _)| other.as_str() != name)
                .map(|(_, (r, _))| *r)
                .collect();
            let point = probe_point(*rect, &others).ok_or_else(|| {
                Stop::infra(format!(
                    "every probe point of {name} is covered by another overlay"
                ))
            })?;
            let (hwnd, pid) = window_at(point);
            observations.insert(
                name.to_string(),
                json!({
                    "rect": rect,
                    "point": [point.0, point.1],
                    "hitWindow": hwnd,
                    "hitPid": pid,
                    "transparentForInput": overlay["native"]["transparentForInput"]
                }),
            );
            verdicts.push((name, point, hwnd, pid));
        }
        ctx.evidence.put("hitTests", observations);
        common::stop_recording(&mut app)?;
        let stimulus_rect = stimulus.rect;
        for (name, point, hwnd, pid) in verdicts {
            if OPERABLE.contains(&name) {
                product_ensure!(
                    pid == product_pid,
                    "a click on {name} at {point:?} reaches process {pid}, not ExoSnap ({product_pid})"
                );
            } else {
                // Only over the stimulus is the window beneath known; elsewhere
                // it could be ExoSnap's own main window.
                infra_ensure!(
                    contains(stimulus_rect, point),
                    "{name} does not lie over the stimulus window, so the window beneath it is unknown"
                );
                product_ensure!(
                    pid != product_pid,
                    "{name} at {point:?} takes the click itself instead of passing it through"
                );
                product_ensure!(
                    hwnd == stimulus.hwnd,
                    "a click through {name} at {point:?} reaches window {hwnd:#x}, not the stimulus beneath it"
                );
            }
        }
        Ok(())
    })();
    stimulus.stop();
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(width: u32, height: u32, fill: u8) -> LumaFrame {
        LumaFrame {
            pts: 0.0,
            width,
            height,
            data: vec![fill; (width * height) as usize],
        }
    }

    #[test]
    fn screen_rectangles_map_onto_the_recorded_monitor() {
        let monitor = [1920, 0, 3840, 1080];
        assert_eq!(
            frame_rect([1920, 0, 2880, 540], monitor, 640, 360),
            Some(Rect {
                x: 0,
                y: 0,
                w: 320,
                h: 180
            })
        );
        assert_eq!(
            frame_rect([3740, 1000, 3940, 1180], monitor, 640, 360).map(|r| (r.x + r.w, r.y + r.h)),
            Some((640, 360)),
            "a rectangle that leaves the monitor is clipped to the frame"
        );
        assert_eq!(frame_rect([0, 0, 100, 100], monitor, 640, 360), None);
    }

    #[test]
    fn luma_difference_is_measured_inside_the_rectangle_only() {
        let reference = frame(8, 8, 100);
        let mut recorded = frame(8, 8, 100);
        for y in 0..4 {
            for x in 0..4 {
                recorded.data[y * 8 + x] = 20;
            }
        }
        let covered = Rect {
            x: 0,
            y: 0,
            w: 4,
            h: 4,
        };
        let clear = Rect {
            x: 4,
            y: 4,
            w: 4,
            h: 4,
        };
        assert_eq!(mean_abs_difference(&recorded, &reference, covered), 80.0);
        assert_eq!(mean_abs_difference(&recorded, &reference, clear), 0.0);
        assert!(mean_abs_difference(&recorded, &reference, covered) > MAX_MEAN_LUMA_DIFFERENCE);
    }

    #[test]
    fn probe_points_avoid_other_overlays() {
        let rect = [0, 0, 100, 40];
        assert_eq!(probe_point(rect, &[]), Some((50, 20)));
        let centre_covered = [[40, 10, 60, 30]];
        let point = probe_point(rect, &centre_covered).unwrap();
        assert!(contains(rect, point) && !contains(centre_covered[0], point));
        assert_eq!(probe_point(rect, &[[-10, -10, 200, 200]]), None);
    }
}
