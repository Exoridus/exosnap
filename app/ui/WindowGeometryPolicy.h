#pragma once

#include <QRect>
#include <QSize>

namespace exosnap::ui {

// Pure policy for restoring persisted window geometry onto a target screen.
//
// The window minimum may exceed the available work area (small displays or
// high scale factors shrink the logical work area below 1120×700); the work
// area is the hard bound, so the clamp range must never invert (VR-001).
//
// `center`: true when the saved position landed on no connected monitor and
// the window is re-centered on the primary screen; false keeps the saved
// position but guarantees a 100×40 px title strip inside the work area.
QRect ClampRestoredWindowGeometry(const QRect& saved, const QRect& available, const QSize& minimum, bool center);

// Full containment clamp applied once on first show: shrinks `window` to fit
// within `available` (never grown) and repositions it so every edge —
// including the bottom edge, which is what a start position under the
// taskbar violates — lies inside the work area. Unlike
// ClampRestoredWindowGeometry this never leaves any edge outside `available`;
// there is no "reachable title strip" allowance, since this runs once at
// startup rather than preserving a user-dragged position.
QRect ClampWindowToWorkArea(const QRect& window, const QRect& available);

struct StartupWindowPlacement {
    QRect rect;
    bool maximized = false;
};

// Where the window opens, given the work area of the screen it will open on.
//
// This is the whole screen-independent half of the startup placement decision:
// choose the rect (the saved one, or `preferred` centred when `saved` is null),
// then apply the two clamps above in the order that matters. Split out from the
// caller that has to find the screen first — quick::ResolveWindowGeometry — so
// the first-launch and restore matrices can be checked against work areas the
// machine running the tests does not have.
//
// `saved_maximized` only counts when there IS a saved rect: a window that has
// never been placed cannot have been maximized. The containment clamp is skipped
// for a maximized restore, because the rect being carried is then only the
// restore rect and the maximized state fills the work area by itself.
StartupWindowPlacement ResolveStartupWindowPlacement(const QRect& saved, bool saved_maximized, const QRect& available,
                                                     const QSize& minimum, const QSize& preferred,
                                                     bool center_on_primary);

} // namespace exosnap::ui
