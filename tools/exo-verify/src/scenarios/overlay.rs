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
            revision: 2,
            title: "Toast and quick controls take clicks; the other overlays let them through",
            class: ScenarioClass::Contract,
            contract: "during a recording the notification toast and quick controls receive clicks inside their input region, while the recording pill passes clicks to another process's window beneath it",
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

/// A window's input region as Windows reports it.
#[derive(Debug, PartialEq)]
enum InputRegion {
    /// `GetWindowRgn` answered ERROR, which means either that no region is set
    /// or that it could not be read. Only produced for a handle already
    /// confirmed to be a live window of the expected process; the evidence
    /// keeps the ambiguity visible.
    Unrestricted,
    /// The region's rectangles in window coordinates. Empty when the region is
    /// set but empty: nothing takes input.
    Parts(Vec<ScreenRect>),
}

impl InputRegion {
    fn evidence(&self) -> Value {
        match self {
            InputRegion::Unrestricted => {
                json!({ "kind": "none-or-unreadable", "parts": null })
            }
            InputRegion::Parts(parts) => json!({ "kind": "region", "parts": parts }),
        }
    }
}

/// Whether a screen point lies in a window's input region.
fn takes_input(rect: ScreenRect, region: &InputRegion, point: (i32, i32)) -> bool {
    match region {
        InputRegion::Unrestricted => contains(rect, point),
        InputRegion::Parts(parts) => parts.iter().any(|part| {
            contains(
                [
                    rect[0] + part[0],
                    rect[1] + part[1],
                    rect[0] + part[2],
                    rect[1] + part[3],
                ],
                point,
            )
        }),
    }
}

/// A point inside `rect` that no other overlay covers and that lies in the
/// window's input region, nearest the centre first. The toast masks the gaps
/// between its cards out of its input region, so a free point of its bounding
/// rectangle can correctly pass a click through; the grid is fine enough to
/// find a card that is much smaller than the window.
fn probe_point(
    rect: ScreenRect,
    others: &[ScreenRect],
    region: &InputRegion,
) -> Option<(i32, i32)> {
    let (w, h) = (rect[2] - rect[0], rect[3] - rect[1]);
    let mut grid: Vec<(i32, i32)> = (1..8)
        .flat_map(|fy| (1..8).map(move |fx| (fx, fy)))
        .collect();
    grid.sort_by_key(|&(fx, fy)| (fx - 4).pow(2) + (fy - 4).pow(2));
    grid.into_iter()
        .map(|(fx, fy)| (rect[0] + w * fx / 8, rect[1] + h * fy / 8))
        .find(|point| {
            !others.iter().any(|other| contains(*other, *point))
                && takes_input(rect, region, *point)
        })
}

/// The overlay's native window handle from its snapshot. Handles are
/// sign-extended 32-bit values, so the product reports them as signed integers.
fn overlay_hwnd(overlay: &Value, name: &str) -> Step<u64> {
    match overlay["native"]["hwnd"].as_i64() {
        Some(hwnd) if hwnd != 0 => Ok(hwnd as u64),
        _ => Err(Stop::infra(format!(
            "{name} snapshot has no non-zero integer native.hwnd"
        ))),
    }
}

/// Runs `f` with physical-pixel coordinates. Overlay geometry is in physical
/// pixels; a DPI-unaware thread would have its coordinates scaled by the system
/// DPI.
#[cfg(windows)]
fn per_monitor_aware<T>(f: impl FnOnce() -> T) -> T {
    use windows::Win32::UI::HiDpi::{
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
    };
    unsafe {
        let previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        let result = f();
        if !previous.is_invalid() {
            SetThreadDpiAwarenessContext(previous);
        }
        result
    }
}

/// The input region Windows holds for `hwnd`. `QWindow::setMask()` is applied
/// as the window region, which both clips drawing and restricts hit-testing.
///
/// Fails when `hwnd` is not a live window of `expected_pid` or the region data
/// cannot be read, so that a failed query is never mistaken for a window
/// without a region.
#[cfg(windows)]
fn input_region(hwnd: u64, expected_pid: u32) -> Result<InputRegion, String> {
    use windows::Win32::Foundation::{HWND, RECT};
    use windows::Win32::Graphics::Gdi::{
        COMPLEXREGION, CreateRectRgn, DeleteObject, GetRegionData, GetWindowRgn, NULLREGION,
        RGNDATAHEADER, SIMPLEREGION,
    };
    use windows::Win32::UI::WindowsAndMessaging::{GetWindowThreadProcessId, IsWindow};
    let window = HWND(hwnd as usize as *mut core::ffi::c_void);
    per_monitor_aware(|| unsafe {
        if !IsWindow(Some(window)).as_bool() {
            return Err(format!("{hwnd:#x} is not a window"));
        }
        let mut pid = 0u32;
        GetWindowThreadProcessId(window, Some(&mut pid));
        if pid != expected_pid {
            return Err(format!(
                "{hwnd:#x} belongs to process {pid}, not {expected_pid}"
            ));
        }
        let region = CreateRectRgn(0, 0, 0, 0);
        if region.is_invalid() {
            return Err("CreateRectRgn failed".to_string());
        }
        let kind = GetWindowRgn(window, region);
        let result = if kind == NULLREGION {
            Ok(InputRegion::Parts(Vec::new()))
        } else if kind == SIMPLEREGION || kind == COMPLEXREGION {
            // u32 storage keeps the RGNDATA header and its RECT array aligned.
            let size = GetRegionData(region, 0, None) as usize;
            let mut buffer = vec![0u32; size.div_ceil(4)];
            let written = if size >= size_of::<RGNDATAHEADER>() {
                GetRegionData(region, size as u32, Some(buffer.as_mut_ptr().cast())) as usize
            } else {
                0
            };
            if written == 0 || written != size {
                Err(format!("GetRegionData failed for {hwnd:#x}"))
            } else {
                let header = &*buffer.as_ptr().cast::<RGNDATAHEADER>();
                let offset = header.dwSize as usize;
                let count = header.nCount as usize;
                if offset + count * size_of::<RECT>() > size {
                    Err(format!(
                        "GetRegionData returned a truncated region for {hwnd:#x}"
                    ))
                } else {
                    let first = buffer.as_ptr().cast::<u8>().add(offset).cast::<RECT>();
                    Ok(InputRegion::Parts(
                        std::slice::from_raw_parts(first, count)
                            .iter()
                            .map(|r| [r.left, r.top, r.right, r.bottom])
                            .collect(),
                    ))
                }
            }
        } else {
            Ok(InputRegion::Unrestricted)
        };
        let _ = DeleteObject(region.into());
        result
    })
}

#[cfg(not(windows))]
fn input_region(_: u64, _: u32) -> Result<InputRegion, String> {
    Err("window regions are only readable on Windows".to_string())
}

/// The top-level window that receives a click at a physical screen point, and
/// its process.
///
/// `WindowFromPoint` performs a hit test, not a click. It can report the window
/// beneath a click-through overlay while real input stays with the overlay. The
/// style assertions in `hit_test_failures` guard the known cause of that gap;
/// neither observes a delivered click.
#[cfg(windows)]
fn window_at((x, y): (i32, i32)) -> (u64, u32) {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::WindowsAndMessaging::{
        GA_ROOT, GetAncestor, GetWindowThreadProcessId, WindowFromPoint,
    };
    per_monitor_aware(|| unsafe {
        let hit = WindowFromPoint(POINT { x, y });
        let root = GetAncestor(hit, GA_ROOT);
        let mut pid = 0u32;
        GetWindowThreadProcessId(root, Some(&mut pid));
        (root.0 as usize as u64, pid)
    })
}

#[cfg(not(windows))]
fn window_at(_: (i32, i32)) -> (u64, u32) {
    (0, 0)
}

struct HitTest {
    name: &'static str,
    point: (i32, i32),
    hit_window: u64,
    hit_pid: u32,
    layered: bool,
    transparent_for_input: bool,
}

fn native_flag(overlay: &Value, name: &str, flag: &str) -> Step<bool> {
    overlay["native"][flag]
        .as_bool()
        .ok_or_else(|| Stop::infra(format!("{name} snapshot has no native.{flag} boolean")))
}

/// Product failures of every overlay, so that one overlay's failure cannot
/// hide another's.
fn hit_test_failures(tests: &[HitTest], product_pid: u32, stimulus_hwnd: u64) -> Vec<String> {
    let mut failures = Vec::new();
    for test in tests {
        let (name, point) = (test.name, test.point);
        if OPERABLE.contains(&name) {
            if test.transparent_for_input {
                failures.push(format!(
                    "{name} is interactive but carries WS_EX_TRANSPARENT"
                ));
            }
            if test.hit_pid != product_pid {
                failures.push(format!(
                    "a click on {name} at {point:?} reaches process {}, not ExoSnap ({product_pid})",
                    test.hit_pid
                ));
            }
        } else {
            // Real cross-process pass-through needs WS_EX_TRANSPARENT paired
            // with WS_EX_LAYERED. Without the layered bit WindowFromPoint still
            // reports the window beneath while a real click stays with the
            // overlay, so the hit test alone cannot catch that regression.
            if !test.transparent_for_input {
                failures.push(format!("{name} is passive but lacks WS_EX_TRANSPARENT"));
            }
            if !test.layered {
                failures.push(format!(
                    "{name} is passive but lacks WS_EX_LAYERED, so real clicks do not pass through to another process"
                ));
            }
            if test.hit_pid == product_pid {
                failures.push(format!(
                    "{name} at {point:?} takes the click itself instead of passing it through"
                ));
            } else if test.hit_window != stimulus_hwnd {
                failures.push(format!(
                    "a click through {name} at {point:?} reaches window {:#x}, not the stimulus beneath it",
                    test.hit_window
                ));
            }
        }
    }
    failures
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
        let mut tests = Vec::new();
        for name in RECORDING_OVERLAYS {
            let (rect, overlay) = &visible[name];
            let others: Vec<ScreenRect> = visible
                .iter()
                .filter(|(other, _)| other.as_str() != name)
                .map(|(_, (r, _))| *r)
                .collect();
            let overlay_hwnd = overlay_hwnd(overlay, name)?;
            let region = input_region(overlay_hwnd, product_pid).map_err(|error| {
                Stop::infra(format!("the input region of {name} is unknown: {error}"))
            })?;
            let point = probe_point(*rect, &others, &region).ok_or_else(|| {
                Stop::infra(format!(
                    "no point of {name} is both free of other overlays and inside its input region"
                ))
            })?;
            let (hit_window, hit_pid) = window_at(point);
            let layered = native_flag(overlay, name, "layered")?;
            let transparent_for_input = native_flag(overlay, name, "transparentForInput")?;
            observations.insert(
                name.to_string(),
                json!({
                    "rect": rect,
                    "inputRegion": region.evidence(),
                    "point": [point.0, point.1],
                    "hitWindow": hit_window,
                    "hitPid": hit_pid,
                    "layered": layered,
                    "transparentForInput": transparent_for_input
                }),
            );
            tests.push(HitTest {
                name,
                point,
                hit_window,
                hit_pid,
                layered,
                transparent_for_input,
            });
        }
        ctx.evidence.put("hitTests", observations);
        common::stop_recording(&mut app)?;
        for test in tests.iter().filter(|test| !OPERABLE.contains(&test.name)) {
            // Only over the stimulus is the window beneath known; elsewhere
            // it could be ExoSnap's own main window.
            infra_ensure!(
                contains(stimulus.rect, test.point),
                "{} does not lie over the stimulus window, so the window beneath it is unknown",
                test.name
            );
        }
        let failures = hit_test_failures(&tests, product_pid, stimulus.hwnd);
        product_ensure!(failures.is_empty(), "{}", failures.join("; "));
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
        let whole = InputRegion::Unrestricted;
        assert_eq!(probe_point(rect, &[], &whole), Some((50, 20)));
        let centre_covered = [[40, 10, 60, 30]];
        let point = probe_point(rect, &centre_covered, &whole).unwrap();
        assert!(contains(rect, point) && !contains(centre_covered[0], point));
        assert_eq!(probe_point(rect, &[[-10, -10, 200, 200]], &whole), None);
    }

    #[test]
    fn probe_points_stay_inside_the_input_region() {
        // Two cards with a click-through gap across the centre, in window
        // coordinates of a window placed at (100, 200).
        let rect = [100, 200, 300, 400];
        let cards = InputRegion::Parts(vec![[0, 0, 200, 60], [0, 140, 200, 200]]);
        let point = probe_point(rect, &[], &cards).unwrap();
        assert!(takes_input(rect, &cards, point));
        assert!(point.1 < 260 || point.1 >= 340, "{point:?} lies in the gap");
        assert_eq!(
            probe_point(rect, &[], &InputRegion::Parts(Vec::new())),
            None,
            "an empty region takes no input anywhere"
        );
    }

    #[test]
    fn a_missing_or_mistyped_handle_is_an_infrastructure_error() {
        for native in [
            json!({}),
            json!({ "hwnd": 0 }),
            json!({ "hwnd": "0x1234" }),
            json!({ "hwnd": 1.5 }),
        ] {
            let overlay = json!({ "native": native });
            assert!(
                matches!(overlay_hwnd(&overlay, TOAST), Err(Stop::Infra(_))),
                "{overlay}"
            );
        }
        let overlay = json!({ "native": { "hwnd": -2 } });
        assert_eq!(overlay_hwnd(&overlay, TOAST).ok(), Some(-2i64 as u64));
    }

    #[cfg(windows)]
    fn hidden_window() -> windows::Win32::Foundation::HWND {
        use windows::Win32::UI::WindowsAndMessaging::{CreateWindowExW, WINDOW_EX_STYLE, WS_POPUP};
        use windows::core::w;
        // Never shown: a hidden window still owns a region.
        unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE(0),
                w!("STATIC"),
                None,
                WS_POPUP,
                0,
                0,
                200,
                200,
                None,
                None,
                None,
                None,
            )
            .expect("create a hidden window")
        }
    }

    #[cfg(windows)]
    #[test]
    fn input_region_reads_back_the_window_region() {
        use windows::Win32::Graphics::Gdi::{
            CombineRgn, CreateRectRgn, DeleteObject, RGN_OR, SetWindowRgn,
        };
        use windows::Win32::UI::WindowsAndMessaging::DestroyWindow;
        let pid = std::process::id();
        unsafe {
            let window = hidden_window();
            let hwnd = window.0 as usize as u64;
            assert_eq!(input_region(hwnd, pid), Ok(InputRegion::Unrestricted));

            let top = CreateRectRgn(0, 0, 200, 60);
            let bottom = CreateRectRgn(0, 140, 200, 200);
            CombineRgn(Some(top), Some(top), Some(bottom), RGN_OR);
            let _ = DeleteObject(bottom.into());
            // The system owns the region after a successful SetWindowRgn.
            assert_ne!(SetWindowRgn(window, Some(top), false), 0);
            assert_eq!(
                input_region(hwnd, pid),
                Ok(InputRegion::Parts(vec![
                    [0, 0, 200, 60],
                    [0, 140, 200, 200]
                ]))
            );

            assert_ne!(
                SetWindowRgn(window, Some(CreateRectRgn(0, 0, 0, 0)), false),
                0
            );
            assert_eq!(
                input_region(hwnd, pid),
                Ok(InputRegion::Parts(Vec::new())),
                "a set but empty region is not the same as no region"
            );
            let _ = DestroyWindow(window);
        }
    }

    #[cfg(windows)]
    #[test]
    fn input_region_refuses_a_handle_it_cannot_vouch_for() {
        use windows::Win32::UI::WindowsAndMessaging::DestroyWindow;
        let pid = std::process::id();
        let window = hidden_window();
        let hwnd = window.0 as usize as u64;
        assert!(
            input_region(hwnd, pid + 1).is_err(),
            "a window of another process is not the overlay"
        );
        unsafe {
            let _ = DestroyWindow(window);
        }
        assert!(
            input_region(hwnd, pid).is_err(),
            "a destroyed window has no region to read"
        );
        assert!(input_region(0, pid).is_err());
    }

    fn hit(name: &'static str, pid: u32, window: u64, layered: bool, transparent: bool) -> HitTest {
        HitTest {
            name,
            point: (0, 0),
            hit_window: window,
            hit_pid: pid,
            layered,
            transparent_for_input: transparent,
        }
    }

    #[test]
    fn every_overlay_is_judged_before_the_verdict() {
        const PRODUCT: u32 = 7;
        const STIMULUS: u64 = 0x42;
        let passing = [
            hit(PILL, 9, STIMULUS, true, true),
            hit(QUICK_CONTROLS, PRODUCT, 1, false, false),
            hit(TOAST, PRODUCT, 2, false, false),
        ];
        assert!(hit_test_failures(&passing, PRODUCT, STIMULUS).is_empty());

        let failing = [
            hit(PILL, PRODUCT, 1, true, true),
            hit(QUICK_CONTROLS, PRODUCT, 1, false, false),
            hit(TOAST, 9, 3, false, false),
        ];
        let failures = hit_test_failures(&failing, PRODUCT, STIMULUS);
        assert_eq!(failures.len(), 2, "{failures:?}");
        assert!(failures[0].contains(PILL) && failures[1].contains(TOAST));
    }

    #[test]
    fn a_passive_overlay_without_the_layered_bit_fails_even_when_the_hit_test_passes() {
        let tests = [hit(PILL, 9, 0x42, false, true)];
        let failures = hit_test_failures(&tests, 7, 0x42);
        assert_eq!(failures.len(), 1);
        assert!(failures[0].contains("WS_EX_LAYERED"));
    }
}
