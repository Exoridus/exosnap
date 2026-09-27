#pragma once

#include <cstdint>
#include <functional>

namespace exosnap {

// What a capture target's monitor looks like to the desktop, resolved from the
// target's native id (an HMONITOR).
//
// Lifted out of RecordPage.cpp's anonymous namespace when the Quick frontend
// needed the same answer for its capture-excluded overlays: those windows have
// to be positioned on the monitor being recorded, and a second implementation
// would be free to disagree with the one the Widgets preview and the region
// overlay already use.
struct ScreenPresentation {
    bool available = false;
    bool primary = false;
    int width = 0;
    int height = 0;
    int origin_x = 0; // rcMonitor.left (virtual-screen coords)
    int origin_y = 0; // rcMonitor.top
};

// Returns an `available == false` result for a null or stale monitor handle
// rather than a zero-sized rectangle, so callers can tell "no such monitor"
// from "a monitor with no extent".
//
// Virtual-screen coordinates, i.e. physical pixels with the primary monitor's
// top-left at the origin. Deliberately NOT Qt logical coordinates: the overlays
// are frameless top-level windows placed against the desktop, and the recorded
// monitor may run a different scale factor than the one the app window is on.
[[nodiscard]] ScreenPresentation QueryScreenPresentation(std::uintptr_t native_id);

// The HMONITOR to query for a capture target: `native_id` as-is for a Monitor
// target, or the hosting monitor of a Window target's HWND via
// MonitorFromWindow(..., MONITOR_DEFAULTTONEAREST). Resolved once per call,
// same as QueryScreenPresentation itself -- a later move to another monitor
// is not tracked, matching FindTargetDisplayFacts's HDR lookup for the same
// target kinds.
[[nodiscard]] std::uintptr_t ResolveTargetMonitor(bool is_window_target, std::uintptr_t native_id);

// Test seam for the Window branch of ResolveTargetMonitor: a real HWND -> real
// monitor lookup is environment state (MONITOR_DEFAULTTONEAREST never fails,
// so a fake test HWND would silently resolve to whatever monitor the test
// machine happens to have), the same reason CaptureExclusion's platform call
// has one.
using WindowMonitorFunction = std::function<std::uintptr_t(std::uintptr_t hwnd)>;
void SetWindowMonitorFunctionForTest(WindowMonitorFunction fn);
void ResetWindowMonitorFunctionForTest();

// A window's on-screen rectangle, virtual-desktop physical pixels, `available
// == false` for a destroyed or otherwise unresolvable HWND.
struct WindowScreenRect {
    bool available = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// DWMWA_EXTENDED_FRAME_BOUNDS rather than GetWindowRect: the latter includes
// the invisible resize border DPI-virtualized windows carry on Windows 10+,
// which disagrees with the visible frame by several pixels per edge. An
// overlay bound to GetWindowRect would sit visibly outside the window it is
// meant to track.
[[nodiscard]] WindowScreenRect QueryWindowScreenRect(std::uintptr_t hwnd);

// Test seam for the Window branch above, same reasoning as
// WindowMonitorFunction: a real HWND query is environment state.
using WindowRectFunction = std::function<WindowScreenRect(std::uintptr_t hwnd)>;
void SetWindowRectFunctionForTest(WindowRectFunction fn);
void ResetWindowRectFunctionForTest();

} // namespace exosnap
