#pragma once

#include <cstdint>

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

} // namespace exosnap
