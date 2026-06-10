#include <gtest/gtest.h>

#include "ui/widgets/RegionGeometry.h"

namespace exosnap::ui::widgets {
namespace {

TEST(RegionGeometryTest, MovePreservesDimensions) {
    const QRect moved = MoveRegionWithinMonitor(QRect(100, 100, 400, 300), QPoint(50, -20), QRect(0, 0, 1000, 800));

    EXPECT_EQ(moved, QRect(150, 80, 400, 300));
}

TEST(RegionGeometryTest, MoveClampsLeftAndTop) {
    const QRect moved = MoveRegionWithinMonitor(QRect(0, 0, 200, 100), QPoint(-500, -500), QRect(-100, -50, 1000, 800));

    EXPECT_EQ(moved, QRect(-100, -50, 200, 100));
}

TEST(RegionGeometryTest, MoveClampsRightAndBottom) {
    const QRect moved = MoveRegionWithinMonitor(QRect(100, 100, 200, 100), QPoint(1000, 1000), QRect(0, 0, 500, 400));

    EXPECT_EQ(moved, QRect(300, 300, 200, 100));
}

TEST(RegionGeometryTest, MoveHandlesNegativeMonitorOrigin) {
    const QRect moved =
        MoveRegionWithinMonitor(QRect(-1800, 100, 300, 200), QPoint(-400, 0), QRect(-1920, 0, 1920, 1080));

    EXPECT_EQ(moved, QRect(-1920, 100, 300, 200));
}

TEST(RegionGeometryTest, ResizeTopLeftAnchorsOppositeCorner) {
    const QRect resized = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::TopLeft, QPoint(50, 70),
                                                 QRect(0, 0, 800, 600), 64);

    EXPECT_EQ(resized, QRect(50, 70, 350, 230));
}

TEST(RegionGeometryTest, ResizeTopRightAnchorsOppositeCorner) {
    const QRect resized = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::TopRight,
                                                 QPoint(450, 80), QRect(0, 0, 800, 600), 64);

    EXPECT_EQ(resized, QRect(100, 80, 350, 220));
}

TEST(RegionGeometryTest, ResizeBottomLeftAnchorsOppositeCorner) {
    const QRect resized = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::BottomLeft,
                                                 QPoint(60, 350), QRect(0, 0, 800, 600), 64);

    EXPECT_EQ(resized, QRect(60, 100, 340, 250));
}

TEST(RegionGeometryTest, ResizeBottomRightAnchorsOppositeCorner) {
    const QRect resized = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::BottomRight,
                                                 QPoint(500, 360), QRect(0, 0, 800, 600), 64);

    EXPECT_EQ(resized, QRect(100, 100, 400, 260));
}

TEST(RegionGeometryTest, ResizeEnforcesMinimumAndPreventsInversion) {
    const QRect resized = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::TopLeft,
                                                 QPoint(1000, 1000), QRect(0, 0, 800, 600), 64);

    EXPECT_EQ(resized, QRect(336, 236, 64, 64));
}

TEST(RegionGeometryTest, ResizeClampsToMonitorBounds) {
    const QRect top_left = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::TopLeft,
                                                  QPoint(-50, -50), QRect(0, 0, 500, 400), 64);
    const QRect bottom_right = ResizeRegionFromCorner(QRect(100, 100, 300, 200), RegionResizeHandle::BottomRight,
                                                      QPoint(999, 999), QRect(0, 0, 500, 400), 64);

    EXPECT_EQ(top_left, QRect(0, 0, 400, 300));
    EXPECT_EQ(bottom_right, QRect(100, 100, 400, 300));
}

TEST(RegionGeometryTest, PresetCentersWhenNoRegionExists) {
    const QRect rect = PresetRegionWithinMonitor(1920, 1080, QRect(0, 0, 2560, 1440));

    EXPECT_EQ(rect, QRect(320, 180, 1920, 1080));
}

TEST(RegionGeometryTest, PresetPreservesExistingCenterWhenPossible) {
    const QRect rect = PresetRegionWithinMonitor(1280, 720, QRect(0, 0, 2560, 1440), QRect(1000, 500, 400, 300));

    EXPECT_EQ(rect, QRect(560, 290, 1280, 720));
}

TEST(RegionGeometryTest, PresetClampsOversizedLandscapeAndPreservesAspect) {
    const QRect rect = PresetRegionWithinMonitor(1920, 1080, QRect(0, 0, 1000, 700));

    EXPECT_EQ(rect, QRect(0, 69, 1000, 562));
}

TEST(RegionGeometryTest, PresetClampsOversizedPortraitAndPreservesAspect) {
    const QRect rect = PresetRegionWithinMonitor(1080, 1920, QRect(0, 0, 1280, 720));

    EXPECT_EQ(rect, QRect(438, 0, 404, 720));
}

TEST(RegionGeometryTest, PresetRejectsUnusableMonitor) {
    const QRect rect = PresetRegionWithinMonitor(1920, 1080, QRect(0, 0, 63, 720));

    EXPECT_FALSE(rect.isValid());
}

TEST(RegionGeometryTest, PresetRejectsOversizedWhenAspectCannotMeetMinimum) {
    const QRect rect = PresetRegionWithinMonitor(1920, 1080, QRect(0, 0, 100, 65));

    EXPECT_FALSE(rect.isValid());
}

TEST(RegionGeometryTest, ClampRejectsTinyRegion) {
    const QRect rect = ClampRegionToMonitor(QRect(0, 0, 63, 720), QRect(0, 0, 1000, 800), 64);

    EXPECT_FALSE(rect.isValid());
}

} // namespace
} // namespace exosnap::ui::widgets
