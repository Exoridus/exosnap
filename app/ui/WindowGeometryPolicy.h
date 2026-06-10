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

} // namespace exosnap::ui
