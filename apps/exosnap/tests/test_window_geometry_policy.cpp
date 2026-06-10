#include "ui/WindowGeometryPolicy.h"

#include <gtest/gtest.h>

namespace exosnap::ui {
namespace {

constexpr QSize kMinimum(1120, 700);

TEST(WindowGeometryPolicy, KeepsSavedGeometryWhenItFits) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect saved(40, 40, 1200, 800);
    EXPECT_EQ(ClampRestoredWindowGeometry(saved, avail, kMinimum, false), saved);
}

TEST(WindowGeometryPolicy, GrowsBelowMinimumWindowUpToMinimum) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect out = ClampRestoredWindowGeometry(QRect(0, 0, 600, 400), avail, kMinimum, false);
    EXPECT_EQ(out.size(), kMinimum);
}

// VR-001: a 1920×1080 work area at 150 % scaling has a logical height of 688,
// below the 700 px window minimum. The work area must win — never an inverted
// clamp range (Debug assertion / UB before the fix).
TEST(WindowGeometryPolicy, WorkAreaSmallerThanMinimumDoesNotInvertBounds) {
    const QRect avail(1706, 236, 1280, 688); // DISPLAY2 at QT_SCALE_FACTOR=1.5
    const QRect out = ClampRestoredWindowGeometry(QRect(2964, 475, 1200, 800), avail, kMinimum, false);
    EXPECT_EQ(out.height(), avail.height());
    EXPECT_EQ(out.width(), 1200);
    EXPECT_TRUE(avail.contains(out.topLeft()));
}

TEST(WindowGeometryPolicy, TinyWorkAreaIsHardBoundInBothDimensions) {
    const QRect avail(0, 0, 683, 384); // 1366×768 at 200 %
    const QRect out = ClampRestoredWindowGeometry(QRect(10, 10, 1200, 800), avail, kMinimum, false);
    EXPECT_EQ(out.size(), avail.size());
    EXPECT_EQ(out.topLeft(), QPoint(10, 10)); // strip is reachable; position preserved
}

TEST(WindowGeometryPolicy, CenteredVariantCentersInsideWorkArea) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect out = ClampRestoredWindowGeometry(QRect(-5000, -5000, 1200, 800), avail, kMinimum, true);
    EXPECT_EQ(out.width(), 1200);
    EXPECT_EQ(out.height(), 800);
    EXPECT_EQ(out.left(), (2560 - 1200) / 2);
    EXPECT_EQ(out.top(), (1392 - 800) / 2);
}

TEST(WindowGeometryPolicy, CenteredVariantSurvivesWorkAreaBelowMinimum) {
    const QRect avail(0, 0, 1280, 696); // primary 2560×1440 at 200 %
    const QRect out = ClampRestoredWindowGeometry(QRect(0, 0, 1200, 800), avail, kMinimum, true);
    EXPECT_EQ(out, QRect(40, 0, 1200, 696));
}

TEST(WindowGeometryPolicy, TitleStripStaysReachable) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect out = ClampRestoredWindowGeometry(QRect(9000, 9000, 1200, 800), avail, kMinimum, false);
    EXPECT_LE(out.left(), avail.right() - 100);
    EXPECT_LE(out.top(), avail.bottom() - 40);
}

} // namespace
} // namespace exosnap::ui
