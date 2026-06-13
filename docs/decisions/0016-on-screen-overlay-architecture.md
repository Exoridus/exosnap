# ADR 0016: On-Screen Overlay Architecture

## Status

Accepted — implementation phased across 0.3.0+.

## Context

ExoSnap needs to surface real-time recording state on-screen (status pill, elapsed
timer, live diagnostics, muted-source glyphs) and deliver transient notifications
(low storage, saved, unexpected stop, recovery available) without contaminating the
recording output. Separately, the webcam PiP is already composited into the video
stream and must remain visually WYSIWYG between the preview and the recorded file.

These two requirements are architecturally distinct. Conflating them into a single
"overlay" concept leads to incorrect design choices: applying display affinity to
the webcam PiP breaks its in-video compositing, while omitting display affinity
from the status pill would burn it into every recording.

The REC status pill (`RecordingOverlayWindow`, shipped PR #52) is the first
production instance of the on-screen-only class.

## Decision

### Two distinct overlay classes

**Class 1 — On-screen-only, excluded from capture**

Rendered as separate top-level DWM-composited Qt windows, positioned in virtual
screen space. Never enter the recording pipeline; no GPU readback; no NVENC cost.
QPainter rendering is sufficient — these are text/glyph widgets with trivial paint
budgets.

Members of this class:

- REC / PAUSED status pill + elapsed timer (shipped, `RecordingOverlayWindow`).
- Live diagnostics readout: fps, A/V drift, dropped frames, output file size
  (`DIAGNOSTICS-OVERLAY-R1`). Driven by the existing `chromeRuntimeMetricsChanged`
  signal and the ~4 Hz stats cadence; does NOT mirror the full Diagnostics page.
  Default: OFF.
- Status glyphs: muted-mic indicator, muted-system-audio indicator, and similar
  transient state symbols.
- NVIDIA-style slide-in notification toasts: low storage, saved, unexpected stop,
  recovery available.

These elements are **never** burned into the recording. There is no "include in
recording" option for class-1 elements; burn-in is exclusively a class-2 feature.

**Class 2 — In-video, GPU-composited into the recording**

Composited onto the captured D3D11 texture on the GPU, inside the VideoThread
(class `GpuCompositor`), before NVENC encoding. Positioned relative to the
recorded frame using normalized `WebcamPlacement` coordinates. The same
`MapWebcamPlacementToContent` helper drives both the live preview and the encoder
so preview geometry is always WYSIWYG with respect to the recorded output.

Members of this class:

- Webcam PiP (shipped, `GpuCompositor::DrawWebcam` shader pass).
- Potential future additions: watermark/logo burn-in, timestamp overlay.

### Mandatory properties for class-1 windows

1. **Click-through:** created with `Qt::WindowTransparentForInput` flag, which sets
   `WS_EX_TRANSPARENT` on the underlying HWND. Class-1 windows must never intercept
   mouse or keyboard input.
2. **Capture exclusion:** `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`
   is applied immediately after the window handle is created. If the call fails
   (unsupported OS build — requires Windows 10 2004 / build 19041), the window hides
   itself and stays hidden for the session. An overlay that cannot guarantee its own
   exclusion must not be visible — contaminating a recording is a silent, hard-to-
   diagnose failure.
3. These two properties are **non-negotiable** for class-1 membership. Any overlay
   that does not satisfy both is a class-2 element (in-video) or must not exist.

### Placement and layering

**Status and diagnostics window (persistent):**
- Anchored to a corner of the **recorded monitor** (currently top-right), not to the
  capture region. Region-level anchoring would cramp small-region recordings; monitor-
  level anchoring is harmless because the window is capture-excluded.
- One persistent `QWidget`-derived window managed alongside `RecordingOverlayWindow`.

**Notification window (transient):**
- Fixed corner of the primary/active display, independent of the recording target.
  Notifications are app-level events, not recording-geometry events.
- A separate transient window or manager handles animation, queuing, and auto-dismiss.
  Notification lifecycle must not interfere with the persistent status/diagnostics
  display: they are separate windows, not layers on the same widget.

**Rationale for separate windows:** persistent status + transient notifications have
incompatible lifecycle requirements. Sharing a window would require the persistent
window to manage a notification queue, blending animation logic with steady-state
rendering. Separate windows keep each responsibility isolated.

### Anti-cheat stance

Class-1 overlays do not inject into the game process, do not hook any API, and do
not read game memory. They are sibling DWM-composited windows — the lowest-risk
overlay category, equivalent to a clock widget on the desktop.

`WDA_EXCLUDEFROMCAPTURE` marks the window as excluded from screen-capture APIs
(WGC, DXGI OD). An always-on-top window with this flag set is not an unusual pattern
(e.g. system tray balloons, screen-reader highlights), and the risk of a well-behaved
anti-cheat system flagging it is low.

However, the risk is nonzero: very aggressive anti-cheat systems (e.g. EasyAntiCheat,
BattlEye, Vanguard) perform kernel-level process/driver enumeration that is not
sensitive to `WDA_EXCLUDEFROMCAPTURE`, and a non-injecting always-on-top window from
an unknown process could in principle trigger a false positive in future detection
heuristics.

**Policy:** no auto-disable. Auto-disabling on anti-cheat detection would create false
positives that damage user trust more than the theoretical anti-cheat flag. Instead:

- An info note in the overlay settings clearly states that no process injection,
  hooking, or memory access occurs.
- A global opt-out toggle disables all class-1 overlays with a single switch.
- An optional lightweight heuristic detects known anti-cheat processes/drivers
  (EasyAntiCheat, BattlEye, Vanguard) at recording start and surfaces a
  **one-time, non-blocking** informational banner: "Anti-cheat detected — ExoSnap
  overlays do not inject; disable overlays if required by the game." No action is
  taken automatically.

### UI convention: ⓘ info-icon-with-hover-popup

Settings that require nuanced explanation (anti-cheat note, advanced options) should
use an ⓘ icon that reveals the explanation in a hover tooltip or inline popup rather
than a permanent sub-label. This keeps the default settings view uncluttered while
making the information discoverable without navigating away.

This convention applies to: the anti-cheat info note on the overlay settings row,
and any other advanced-settings row where a sub-label would exceed one short line.

## Consequences

- **`DIAGNOSTICS-OVERLAY-R1` (0.3.0 slice — IMPLEMENTED):** stats text (fps/bitrate,
  A/V drift, dropped frames, output size) + status glyphs (muted mic, muted sys).
  Default OFF. Driven by `chromeRuntimeMetricsChanged` + `audioMeterLevelsUpdated`
  at the ~4–30 Hz cadence already on the wire.
  Implemented as a **sibling top-level window** (`DiagnosticsOverlayWindow`) anchored
  to the **bottom-right** corner of the recorded monitor; `RecordingOverlayWindow`
  (status pill) occupies top-right so the two windows are non-overlapping.
  Toggle lives in AdvancedPage alongside the existing recording-overlay toggle.
  Persisted as `show_diagnostics_overlay` (default `false`) in the `[overlay]`
  settings group; settings version bumped to 8.

- **Notification overlay (separate 0.3.0 slice):** tied to the 0.3.0 notification
  center. Anchored to the primary display. Implements queue + auto-dismiss. Does not
  share state with the status/diagnostics window.

- **Class-2 additions (watermark, timestamp):** follow the existing `GpuCompositor`
  shader-pass pattern. `WebcamPlacement` normalized coordinates are reused where
  applicable. No class-1 mechanism (display affinity, QPainter) is involved.

- **No class-1 burn-in path.** If a user wants a timestamp or logo in the recorded
  video, that is a class-2 feature (in-video compositor pass). There is no mechanism
  to opt a class-1 element into the recording.

- **`SetWindowDisplayAffinity` failure handling** is already implemented in
  `RecordingOverlayWindow::applyExclusion()` and is the mandatory pattern for all
  future class-1 windows.

- **Windows 10 2004+ requirement** for `WDA_EXCLUDEFROMCAPTURE` is already a
  documented ExoSnap system requirement; no new constraint is introduced.
