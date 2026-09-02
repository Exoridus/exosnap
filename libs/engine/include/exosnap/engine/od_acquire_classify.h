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

// Whether a poll that produced no desktop frame should enter the duplication
// recovery hold (the one ACCESS_LOST uses) instead of being taken for an idle
// desktop.
//
// A display mode or topology change does not reliably invalidate a duplication:
// it can leave one that keeps returning DXGI_ERROR_WAIT_TIMEOUT forever, which
// looks exactly like a desktop nobody is touching. Elapsed time cannot separate
// the two -- an idle desktop is legitimately silent for minutes, and a recovery
// driven by "no frame for N seconds" would rebuild the duplication of a
// perfectly healthy static desktop over and over. Changed topology is the
// evidence that does separate them, and reopening clears it, so recovery runs
// once per change and never on a static desktop.
[[nodiscard]] constexpr bool ShouldRecoverIdleOdAcquire(bool already_recovering, bool topology_changed) noexcept {
    return !already_recovering && topology_changed;
}

} // namespace exosnap::engine
