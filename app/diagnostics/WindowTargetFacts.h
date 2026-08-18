#pragma once

// Honest, elevation-free detection of a window capture target that an
// exclusive-fullscreen (legacy FSE) application will render black.
//
// Two layers, kept apart so the decision logic is unit-pinned without Win32:
//   * WindowTargetFacts   — a flat snapshot of a window's geometry/style/state,
//                           gathered from Win32 by GatherWindowTargetFacts().
//   * ClassifyWindowShape / CombineFullscreenEvidence — PURE resolvers over that
//                           snapshot plus measured hub evidence. No Win32 calls,
//                           no wall clock, no judgement about *how* facts were
//                           obtained.
//
// The shape heuristic (window covers its monitor AND has no caption/resize
// frame) cannot tell borderless from FSE — both look "fullscreen-shaped" — so it
// is only ever a weak signal. A card is raised only with positive extra evidence:
// a fullscreen signal (QUNS / PresentMon) for a Suspected notice, or measured hub
// evidence (WGC produced nothing / froze at the shape transition) for a
// ProvenBlack blocker.

#include <optional>

#include <windows.h>

#include <recorder_core/capture_hub_policy.h> // recorder_core::HubFrameKind

namespace exosnap::diagnostics {

// A flat snapshot of a window capture target. Gathered once by
// GatherWindowTargetFacts(); everything downstream reasons over this struct so
// the logic is testable without a real HWND.
struct WindowTargetFacts {
    bool valid = false;         // handle is a live window
    bool visible = false;       // IsWindowVisible
    bool minimized = false;     // IsIconic
    bool cloaked = false;       // DWMWA_CLOAKED (virtual-desktop / UWP-suspended)
    bool is_foreground = false; // GetForegroundWindow == target
    RECT window_rect{};         // GetWindowRect (screen coords)
    RECT monitor_rect{};        // MONITORINFO.rcMonitor of MonitorFromWindow
    LONG_PTR style = 0;         // GWL_STYLE
    LONG_PTR ex_style = 0;      // GWL_EXSTYLE
    // SHQueryUserNotificationState() == QUNS_RUNNING_D3D_FULL_SCREEN. Only
    // meaningful for the primary monitor; a documented Shell signal, no elevation.
    bool quns_d3d_fullscreen = false;
};

// Measured evidence from the selected-window WGC probe (S2a). A pure snapshot —
// no Qt, no COM — copied out of WindowEvidenceProbe under its mutex.
struct WindowHubEvidence {
    recorder_core::HubFrameKind kind = recorder_core::HubFrameKind::None;
    double seconds_subscribed = 0.0;        // how long the probe has been subscribed
    double seconds_since_fresh_frame = 0.0; // since the last new frame (Held only)
    // A fresh frame arrived AFTER the window was observed to become
    // FullscreenShaped. Distinguishes a window that froze *at* the shape
    // transition (FSE black) from a legitimately static borderless window
    // (paused fullscreen video) that kept producing frames after the transition.
    bool fresh_frame_since_fullscreen_shape = false;
};

enum class WindowShape {
    Normal,           // has a caption/frame, or does not cover its monitor
    FullscreenShaped, // covers its monitor and has no caption/resize frame
};

enum class ExclusiveEvidence {
    None,        // nothing to report (also the borderless-that-works case)
    Suspected,   // fullscreen-shaped + a fullscreen signal (QUNS/PresentMon) -> Notice
    ProvenBlack, // fullscreen-shaped + measured proof capture yields nothing -> Blocker
};

// Evidence must accumulate for at least this long before it counts. WGC always
// delivers an initial frame of the current content at session start (even for a
// fully static window), so a window that is *still* None after this window has no
// DWM surface — i.e. it is in FSE.
inline constexpr double kEvidenceMinSeconds = 2.0;

// PURE. FullscreenShaped iff the window is a live, visible, non-minimized window
// whose rect covers its whole monitor and which carries neither a title-bar
// caption (WS_CAPTION) nor a sizing frame (WS_THICKFRAME). Borderless and FSE
// are indistinguishable here by design.
[[nodiscard]] WindowShape ClassifyWindowShape(const WindowTargetFacts& facts) noexcept;

// PURE. Combine the shape with measured hub evidence and an external fullscreen
// signal (QUNS d3d-fullscreen OR PresentMon ExclusiveFullscreen, OR'd by the
// caller) into the severity ladder:
//   shape != FullscreenShaped                       -> None
//   ProvenBlack: kind==None && subscribed >= 2 s, OR
//                kind==Held && no fresh frame since the shape transition
//                            && seconds_since_fresh_frame >= 2 s
//   else fullscreen_signal                          -> Suspected
//   else                                            -> None
[[nodiscard]] ExclusiveEvidence CombineFullscreenEvidence(WindowShape shape, const WindowHubEvidence& hub,
                                                          bool fullscreen_signal) noexcept;

// PURE. The whole verdict for one selected window target, from the raw probe
// snapshot: classify the shape, OR the two fullscreen signals (the window's own
// QUNS state and an optional PresentMon ExclusiveFullscreen observation), then
// combine. This exists so the Diagnostics card and the recording-admission gate
// cannot drift: both consume the same probe snapshot through this one function
// rather than each re-assembling the ladder from its parts.
[[nodiscard]] ExclusiveEvidence ResolveExclusiveEvidence(const WindowTargetFacts& facts, const WindowHubEvidence& hub,
                                                         bool present_exclusive_fullscreen) noexcept;

// Thin Win32 gatherer. Reads geometry/style/state + SHQueryUserNotificationState.
// Facts only, no judgement. A null / dead HWND yields valid == false.
[[nodiscard]] WindowTargetFacts GatherWindowTargetFacts(HWND hwnd);

} // namespace exosnap::diagnostics
