#include "WindowGeometryPolicy.h"

#include <algorithm>

namespace exosnap::ui {

QRect ClampRestoredWindowGeometry(const QRect& saved, const QRect& available, const QSize& minimum, bool center) {
    const int min_w = std::min(minimum.width(), available.width());
    const int min_h = std::min(minimum.height(), available.height());
    const int w = std::clamp(saved.width(), min_w, available.width());
    const int h = std::clamp(saved.height(), min_h, available.height());

    if (center) {
        return {available.left() + (available.width() - w) / 2, available.top() + (available.height() - h) / 2, w, h};
    }

    // Keep at least a 100×40 px strip of the window inside the available area.
    const int max_x = std::max(available.left(), available.right() - std::min(w, 100));
    const int max_y = std::max(available.top(), available.bottom() - 40);
    const int x = std::clamp(saved.x(), available.left(), max_x);
    const int y = std::clamp(saved.y(), available.top(), max_y);
    return {x, y, w, h};
}

QRect ClampWindowToWorkArea(const QRect& window, const QRect& available) {
    const int w = std::min(window.width(), available.width());
    const int h = std::min(window.height(), available.height());
    // available.right()/bottom() are inclusive (x+width-1); the max origin that
    // still keeps the shrunk rect fully inside is available.left()+available.width()-w.
    const int x = std::clamp(window.x(), available.left(), available.left() + available.width() - w);
    const int y = std::clamp(window.y(), available.top(), available.top() + available.height() - h);
    return {x, y, w, h};
}

StartupWindowPlacement ResolveStartupWindowPlacement(const QRect& saved, bool saved_maximized, const QRect& available,
                                                     const QSize& minimum, const QSize& preferred,
                                                     bool center_on_primary) {
    const bool has_saved_rect = saved.width() > 0 && saved.height() > 0;

    StartupWindowPlacement placement;
    placement.maximized = has_saved_rect && saved_maximized;

    const QSize size = has_saved_rect ? saved.size() : preferred;
    const QRect requested =
        has_saved_rect ? saved : QRect(available.center() - QPoint(size.width() / 2, size.height() / 2), size);

    // Two stages, exactly as the Widgets restore did. The first preserves a
    // deliberately-placed window while guaranteeing a reachable title strip; the
    // second is the full containment clamp, which is what stops a bottom edge
    // ending up under the taskbar.
    QRect rect = ClampRestoredWindowGeometry(requested, available, minimum, center_on_primary);
    if (!placement.maximized)
        rect = ClampWindowToWorkArea(rect, available);
    placement.rect = rect;
    return placement;
}

} // namespace exosnap::ui
