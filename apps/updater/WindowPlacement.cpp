#include "WindowPlacement.h"

namespace exosnap::updater {

QRect PlaceWindowNearAnchor(QSize window_size, QPoint anchor_center, QRect available) {
    QRect target(anchor_center.x() - window_size.width() / 2, anchor_center.y() - window_size.height() / 2,
                 window_size.width(), window_size.height());

    // Slide back inside `available` on whichever edge(s) overflow. Order
    // matters when the window is wider/taller than the available area itself
    // (a tiny or oddly-shaped monitor work area): clamping the leading edge
    // after the trailing edge always wins, pinning to available's origin
    // rather than leaving the window hanging off the far edge.
    if (target.right() > available.right())
        target.moveRight(available.right());
    if (target.left() < available.left())
        target.moveLeft(available.left());
    if (target.bottom() > available.bottom())
        target.moveBottom(available.bottom());
    if (target.top() < available.top())
        target.moveTop(available.top());

    return target;
}

} // namespace exosnap::updater