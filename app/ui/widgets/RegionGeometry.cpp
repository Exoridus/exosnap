#include "RegionGeometry.h"

#include <algorithm>
#include <cmath>

namespace exosnap::ui::widgets {
namespace {

int RightEdge(const QRect& rect) {
    return rect.x() + rect.width();
}

int BottomEdge(const QRect& rect) {
    return rect.y() + rect.height();
}

bool HasUsableBounds(const QRect& monitor, int min_dimension) {
    return monitor.width() >= min_dimension && monitor.height() >= min_dimension;
}

} // namespace

QRect ClampRegionToMonitor(const QRect& region, const QRect& monitor, int min_dimension) {
    if (!HasUsableBounds(monitor, min_dimension) || region.width() < min_dimension || region.height() < min_dimension) {
        return {};
    }

    const int width = std::min(region.width(), monitor.width());
    const int height = std::min(region.height(), monitor.height());
    const int x = std::clamp(region.x(), monitor.x(), RightEdge(monitor) - width);
    const int y = std::clamp(region.y(), monitor.y(), BottomEdge(monitor) - height);
    return QRect(x, y, width, height);
}

QRect MoveRegionWithinMonitor(const QRect& region, const QPoint& delta, const QRect& monitor) {
    if (region.isEmpty() || monitor.isEmpty()) {
        return {};
    }

    QRect moved(region.x() + delta.x(), region.y() + delta.y(), region.width(), region.height());
    return ClampRegionToMonitor(moved, monitor, std::min(region.width(), region.height()));
}

QRect ResizeRegionFromCorner(const QRect& region, RegionResizeHandle handle, const QPoint& cursor, const QRect& monitor,
                             int min_dimension) {
    if (!HasUsableBounds(monitor, min_dimension) || region.width() < min_dimension || region.height() < min_dimension) {
        return {};
    }

    const int left = region.x();
    const int top = region.y();
    const int right = RightEdge(region);
    const int bottom = BottomEdge(region);
    const int monitor_left = monitor.x();
    const int monitor_top = monitor.y();
    const int monitor_right = RightEdge(monitor);
    const int monitor_bottom = BottomEdge(monitor);

    int new_left = left;
    int new_top = top;
    int new_right = right;
    int new_bottom = bottom;

    switch (handle) {
    case RegionResizeHandle::TopLeft:
        new_left = std::clamp(cursor.x(), monitor_left, right - min_dimension);
        new_top = std::clamp(cursor.y(), monitor_top, bottom - min_dimension);
        break;
    case RegionResizeHandle::TopRight:
        new_right = std::clamp(cursor.x(), left + min_dimension, monitor_right);
        new_top = std::clamp(cursor.y(), monitor_top, bottom - min_dimension);
        break;
    case RegionResizeHandle::BottomLeft:
        new_left = std::clamp(cursor.x(), monitor_left, right - min_dimension);
        new_bottom = std::clamp(cursor.y(), top + min_dimension, monitor_bottom);
        break;
    case RegionResizeHandle::BottomRight:
        new_right = std::clamp(cursor.x(), left + min_dimension, monitor_right);
        new_bottom = std::clamp(cursor.y(), top + min_dimension, monitor_bottom);
        break;
    case RegionResizeHandle::None:
    default:
        break;
    }

    return QRect(new_left, new_top, new_right - new_left, new_bottom - new_top);
}

QRect PresetRegionWithinMonitor(int preset_width, int preset_height, const QRect& monitor, const QRect& existing_region,
                                int min_dimension) {
    if (preset_width <= 0 || preset_height <= 0 || !HasUsableBounds(monitor, min_dimension)) {
        return {};
    }

    double width = preset_width;
    double height = preset_height;
    if (width > monitor.width() || height > monitor.height()) {
        const double scale =
            std::min(static_cast<double>(monitor.width()) / width, static_cast<double>(monitor.height()) / height);
        width *= scale;
        height *= scale;
    }

    const int resolved_width = std::min(static_cast<int>(std::floor(width)) & ~1, monitor.width());
    const int resolved_height = std::min(static_cast<int>(std::floor(height)) & ~1, monitor.height());
    if (resolved_width < min_dimension || resolved_height < min_dimension) {
        return {};
    }

    const bool preserve_existing_center = existing_region.isValid() && existing_region.width() >= min_dimension &&
                                          existing_region.height() >= min_dimension &&
                                          monitor.intersects(existing_region);
    const int center_x = preserve_existing_center ? existing_region.x() + existing_region.width() / 2
                                                  : monitor.x() + monitor.width() / 2;
    const int center_y = preserve_existing_center ? existing_region.y() + existing_region.height() / 2
                                                  : monitor.y() + monitor.height() / 2;

    const int x = std::clamp(center_x - resolved_width / 2, monitor.x(), RightEdge(monitor) - resolved_width);
    const int y = std::clamp(center_y - resolved_height / 2, monitor.y(), BottomEdge(monitor) - resolved_height);
    return QRect(x, y, resolved_width, resolved_height);
}

} // namespace exosnap::ui::widgets
