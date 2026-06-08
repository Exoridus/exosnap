#pragma once

#include <QPoint>
#include <QRect>

namespace exosnap::ui::widgets {

enum class RegionResizeHandle {
    None,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

QRect ClampRegionToMonitor(const QRect& region, const QRect& monitor, int min_dimension);
QRect MoveRegionWithinMonitor(const QRect& region, const QPoint& delta, const QRect& monitor);
QRect ResizeRegionFromCorner(const QRect& region, RegionResizeHandle handle, const QPoint& cursor, const QRect& monitor,
                             int min_dimension);
QRect PresetRegionWithinMonitor(int preset_width, int preset_height, const QRect& monitor,
                                const QRect& existing_region = QRect(), int min_dimension = 64);

} // namespace exosnap::ui::widgets
