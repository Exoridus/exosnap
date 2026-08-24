#pragma once
#include <cstdint>

namespace exosnap::engine {

// What one successful DXGI Output Duplication AcquireNextFrame actually
// delivered. DXGI can wake the acquire with only a cursor move/visibility/
// shape change and LastPresentTime == 0 — treating that as a fresh desktop
// frame (the pre-existing behavior this replaces) causes an unnecessary
// screen-texture copy, phase-correct pacing-ring entry, and downstream
// composition/conversion for a frame that never actually changed on screen.
enum class OdAcquireKind : uint8_t {
    DesktopPresent, // LastPresentTime != 0: a real, new desktop frame.
    CursorOnly,     // No desktop frame, but cursor state changed and capture_cursor is on.
    Ignorable,      // Nothing actionable: no present, and no mouse update (or cursor capture is off).
};

[[nodiscard]] constexpr OdAcquireKind ClassifyOdAcquire(bool has_present, bool has_mouse_update,
                                                        bool cursor_capture_enabled) noexcept {
    if (has_present)
        return OdAcquireKind::DesktopPresent;
    if (has_mouse_update && cursor_capture_enabled)
        return OdAcquireKind::CursorOnly;
    return OdAcquireKind::Ignorable;
}

} // namespace exosnap::engine
