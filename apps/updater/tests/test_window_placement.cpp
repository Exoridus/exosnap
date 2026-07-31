// test_window_placement.cpp -- pure geometry for anchoring the updater window
// to the ExoSnap window it was launched for, clamped inside the target
// monitor's available (taskbar-excluded) work area.

#include <gtest/gtest.h>

#include <cstdlib>

#include "WindowPlacement.h"

namespace {

// A generous single-monitor work area, no taskbar overlap to worry about.
constexpr QRect kRoomyAvailable(0, 0, 1920, 1040); // 1080 minus a 40px taskbar

// QRect's right()/bottom() are inclusive edges, so an even-sized rect's
// center() rounds down by up to 1px from the true midpoint -- not a placement
// bug, just integer rect arithmetic. Assertions below tolerate that.
::testing::AssertionResult NearlyEqual(QPoint a, QPoint b, int tolerance = 1) {
    if (std::abs(a.x() - b.x()) <= tolerance && std::abs(a.y() - b.y()) <= tolerance)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "expected " << a.x() << "," << a.y() << " to be within " << tolerance
                                         << "px of " << b.x() << "," << b.y();
}

TEST(WindowPlacement, CentersOnAnchorWhenThereIsRoom) {
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(960, 520), kRoomyAvailable);
    EXPECT_TRUE(NearlyEqual(r.center(), QPoint(960, 520)));
    EXPECT_EQ(r.size(), QSize(400, 300));
}

TEST(WindowPlacement, ClampsRightEdgeInsteadOfOverflowing) {
    // Anchor near the right edge of the monitor -- a naive center would push
    // the window's right edge past available.right().
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(1900, 520), kRoomyAvailable);
    EXPECT_LE(r.right(), kRoomyAvailable.right());
    EXPECT_GE(r.left(), kRoomyAvailable.left());
}

TEST(WindowPlacement, ClampsLeftEdgeInsteadOfOverflowing) {
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(20, 520), kRoomyAvailable);
    EXPECT_GE(r.left(), kRoomyAvailable.left());
    EXPECT_LE(r.right(), kRoomyAvailable.right());
}

TEST(WindowPlacement, ClampsBottomEdgeAboveTheTaskbar) {
    // Anchor near the bottom of the available area (just above where a
    // taskbar would start) -- the window must not hang below it.
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(960, 1030), kRoomyAvailable);
    EXPECT_LE(r.bottom(), kRoomyAvailable.bottom());
    EXPECT_GE(r.top(), kRoomyAvailable.top());
}

TEST(WindowPlacement, ClampsTopEdgeInsteadOfOverflowing) {
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(960, 10), kRoomyAvailable);
    EXPECT_GE(r.top(), kRoomyAvailable.top());
    EXPECT_LE(r.bottom(), kRoomyAvailable.bottom());
}

TEST(WindowPlacement, TopLeftCornerAnchorStaysFullyInsideAvailable) {
    // Worst case: anchor exactly at the corner overflows two edges at once.
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(0, 0), kRoomyAvailable);
    EXPECT_TRUE(kRoomyAvailable.contains(r));
}

TEST(WindowPlacement, BottomRightCornerAnchorStaysFullyInsideAvailable) {
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(kRoomyAvailable.right(), kRoomyAvailable.bottom()),
                                          kRoomyAvailable);
    EXPECT_TRUE(kRoomyAvailable.contains(r));
}

// A secondary monitor to the left of the primary, its own taskbar-adjusted
// work area -- the anchor point and available rect both live in that
// monitor's coordinate space (Windows virtual-desktop coordinates can be
// negative), proving the math isn't accidentally anchored to (0,0).
TEST(WindowPlacement, WorksInASecondaryMonitorsCoordinateSpace) {
    const QRect secondary_available(-1920, 0, 1920, 1040);
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(-960, 520), secondary_available);
    EXPECT_TRUE(NearlyEqual(r.center(), QPoint(-960, 520)));
    EXPECT_TRUE(secondary_available.contains(r));
}

// A window larger than the available area in one dimension (a tiny or
// oddly-shaped work area) can't be made to fit -- it should pin to the
// available area's origin on that axis rather than hang off an edge.
TEST(WindowPlacement, OversizedWindowPinsToAvailableOrigin) {
    const QRect tiny_available(100, 100, 200, 150);
    const QRect r = PlaceWindowNearAnchor(QSize(400, 300), QPoint(200, 175), tiny_available);
    EXPECT_EQ(r.left(), tiny_available.left());
    EXPECT_EQ(r.top(), tiny_available.top());
}

} // namespace
