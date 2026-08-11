#include "ui/WindowGeometryPolicy.h"

#include "ui/theme/ExoSnapMetrics.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace exosnap::ui {
namespace {

constexpr QSize kMinimum(1120, 700);

// ── First-launch placement ──────────────────────────────────────────────────
//
// The shipped minimum, and the preferred size the window opens at when nothing
// has been persisted. The two clamps below are the whole of what
// QuickApplication's ResolveWindowGeometry does on that path, so exercising them
// with the real constants is a genuine check of first-launch placement rather
// than of a number copied into a test.
constexpr QSize kShippedMinimum(theme::ExoSnapMetrics::kMinWindowWidth, theme::ExoSnapMetrics::kMinWindowHeight);
constexpr QSize kPreferred(theme::ExoSnapMetrics::kPreferredWindowWidth, theme::ExoSnapMetrics::kPreferredWindowHeight);

// The real "no persisted rect" branch, not a re-implementation of it. This used
// to centre and clamp by hand here, which meant the tests could stay green while
// the shipped resolver drifted away from them.
QRect FirstLaunchRect(const QRect& available) {
    return ResolveStartupWindowPlacement(QRect(), false, available, kShippedMinimum, kPreferred, true).rect;
}

TEST(FirstLaunchGeometry, PreferredSizeIsAboveTheShippedMinimum) {
    EXPECT_GE(kPreferred.width(), kShippedMinimum.width());
    EXPECT_GE(kPreferred.height(), kShippedMinimum.height());
}

// 2560x1440 at 100 %, taskbar reserved.
TEST(FirstLaunchGeometry, FitsAndCentersOn2560x1440) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect out = FirstLaunchRect(avail);
    EXPECT_EQ(out.size(), kPreferred);
    EXPECT_TRUE(avail.contains(out));
    EXPECT_LE(std::abs(out.center().x() - avail.center().x()), 1);
    EXPECT_LE(std::abs(out.center().y() - avail.center().y()), 1);
}

// 1920x1080 at 100 %, taskbar reserved.
TEST(FirstLaunchGeometry, FitsAndCentersOn1920x1080) {
    const QRect avail(0, 0, 1920, 1032);
    const QRect out = FirstLaunchRect(avail);
    EXPECT_EQ(out.size(), kPreferred);
    EXPECT_TRUE(avail.contains(out));
    EXPECT_LE(std::abs(out.center().x() - avail.center().x()), 1);
}

// 1366x768 class at 100 %: the work area is only 720 px tall, exactly the
// preferred height, and 1366 px wide. Nothing may hang off any edge — this is
// the size where a first launch used to start under the taskbar.
TEST(FirstLaunchGeometry, FullyContainedOn1366x768ClassWorkArea) {
    const QRect avail(0, 0, 1366, 720);
    const QRect out = FirstLaunchRect(avail);
    EXPECT_TRUE(avail.contains(out));
    EXPECT_EQ(out.width(), kPreferred.width());
    EXPECT_EQ(out.height(), 720);
}

// 2560x1440 at 150 % scaling -> a 1706x912 logical screen, taskbar reserved.
TEST(FirstLaunchGeometry, FitsOn2560x1440At150Percent) {
    const QRect avail(0, 0, 1706, 880);
    const QRect out = FirstLaunchRect(avail);
    EXPECT_EQ(out.size(), kPreferred);
    EXPECT_TRUE(avail.contains(out));
}

// 1920x1080 at 150 % -> a 1280x688 logical work area: narrower than the
// preferred width and SHORTER than the 700 px window minimum (VR-001). The work
// area is the hard bound in both directions, and the window still has to land
// fully inside it rather than being grown to a minimum that does not fit.
TEST(FirstLaunchGeometry, WorkAreaWinsOn1920x1080At150Percent) {
    const QRect avail(0, 0, 1280, 688);
    const QRect out = FirstLaunchRect(avail);
    EXPECT_EQ(out, avail);
}

// A persisted rect always outranks the preferred size, including one that is
// smaller than it.
TEST(FirstLaunchGeometry, PersistedRectOutranksThePreferredSize) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect saved(300, 200, 980, 780);
    EXPECT_EQ(ClampWindowToWorkArea(ClampRestoredWindowGeometry(saved, avail, kShippedMinimum, false), avail), saved);
}

// ── Startup placement ───────────────────────────────────────────────────────
//
// The rect the window is created at and shown on. Everything here is what the
// user sees on the FIRST frame, so a wrong answer is not corrected later — the
// startup lifecycle deliberately has nothing left to correct with.

TEST(StartupWindowPlacement, RestoresASavedRectUnchangedWhenItFits) {
    const QRect avail(0, 0, 2560, 1392);
    const QRect saved(400, 120, 1280, 820);
    const StartupWindowPlacement out =
        ResolveStartupWindowPlacement(saved, false, avail, kShippedMinimum, kPreferred, false);
    EXPECT_EQ(out.rect, saved);
    EXPECT_FALSE(out.maximized);
}

// The saved rect is the RESTORE rect of a maximized window, so it survives
// intact: it is not the rect the window opens on, and clamping it to the work
// area would silently rewrite where un-maximizing lands.
TEST(StartupWindowPlacement, MaximizedRestoreKeepsItsRestoreRect) {
    const QRect avail(0, 0, 1920, 1032);
    const QRect saved(200, 100, 1280, 820);
    const StartupWindowPlacement out =
        ResolveStartupWindowPlacement(saved, true, avail, kShippedMinimum, kPreferred, false);
    EXPECT_TRUE(out.maximized);
    EXPECT_EQ(out.rect, saved);
}

// A first launch has no saved rect and therefore cannot be maximized, whatever
// a stale persisted flag claims.
TEST(StartupWindowPlacement, FirstLaunchIsNeverMaximized) {
    const QRect avail(0, 0, 2560, 1392);
    const StartupWindowPlacement out =
        ResolveStartupWindowPlacement(QRect(), true, avail, kShippedMinimum, kPreferred, true);
    EXPECT_FALSE(out.maximized);
    EXPECT_EQ(out.rect.size(), kPreferred);
}

// A monitor that has been unplugged since the last run: the position is
// abandoned and the window is re-centred rather than restored to coordinates
// nothing can display.
TEST(StartupWindowPlacement, RecentersWhenTheSavedPositionIsUnreachable) {
    const QRect avail(0, 0, 1920, 1032);
    const QRect saved(4200, 300, 1280, 820);
    const StartupWindowPlacement out =
        ResolveStartupWindowPlacement(saved, false, avail, kShippedMinimum, kPreferred, true);
    EXPECT_TRUE(avail.contains(out.rect));
    EXPECT_LE(std::abs(out.rect.center().x() - avail.center().x()), 1);
}

// Restoring the same rect must be a fixed point. This is the property the
// launch-to-launch drift violated: each start returned a slightly different rect
// from the one it was given, and that rect was persisted and fed back in.
TEST(StartupWindowPlacement, RestoreIsAFixedPoint) {
    const QRect avail(0, 0, 2560, 1392);
    QRect rect(400, 120, 1280, 820);
    for (int start = 0; start < 4; ++start)
        rect = ResolveStartupWindowPlacement(rect, false, avail, kShippedMinimum, kPreferred, false).rect;
    EXPECT_EQ(rect, QRect(400, 120, 1280, 820));
}

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

// ---------------------------------------------------------------------------
// ClampWindowToWorkArea (B3: first-show taskbar clamp)
//
// Unlike ClampRestoredWindowGeometry (which only guarantees a reachable title
// strip so a user-dragged position surviving a monitor swap isn't yanked back
// on screen), this is a full containment clamp applied once on first show:
// position AND size are corrected so the window never has any edge — in
// particular the bottom edge — resting under the taskbar / outside the work
// area.
// ---------------------------------------------------------------------------

TEST(ClampWindowToWorkAreaTest, BottomOverhangIsPulledFullyInside) {
    // Window bottom edge hangs 80 px below the work area (taskbar covers it).
    const QRect avail(0, 0, 1920, 1040);
    const QRect window(100, 520, 800, 600); // bottom = 1120, 80 px past avail.bottom()+1 (1040)
    const QRect out = ClampWindowToWorkArea(window, avail);
    EXPECT_TRUE(avail.contains(out));
}

TEST(ClampWindowToWorkAreaTest, LargerThanWorkAreaIsShrunkToFit) {
    const QRect avail(0, 0, 1920, 1040);
    const QRect window(0, 0, 2000, 1200);
    const QRect out = ClampWindowToWorkArea(window, avail);
    EXPECT_EQ(out, avail);
}

TEST(ClampWindowToWorkAreaTest, WindowAlreadyInsideIsUnchanged) {
    const QRect avail(0, 0, 1920, 1040);
    const QRect window(100, 100, 800, 600);
    const QRect out = ClampWindowToWorkArea(window, avail);
    EXPECT_EQ(out, window);
}

// Caller-consistency guard: the clamp is pure rect arithmetic, so it must treat a
// non-primary monitor's work area (non-zero origin, e.g. a second 1920x1040 screen
// sitting to the right of the primary) exactly like the primary's origin-anchored
// one. A window already fully inside SCREEN 2's work area must come back unchanged
// — this is the caller-side property MainWindow::applyRestoredGeometry() relies on
// when it passes the resolved target screen's availableGeometry() (not
// this->screen(), which can still report the primary screen immediately after
// setGeometry() moves the window cross-monitor, before Qt/Windows updates the
// QWindow->QScreen association).
TEST(ClampWindowToWorkAreaTest, SecondMonitorWithNonZeroOriginRectStaysUnchanged) {
    const QRect avail(1920, 0, 1920, 1040);  // Screen 2, to the right of a 1920-wide primary
    const QRect window(2100, 100, 800, 600); // fully inside Screen 2's work area
    const QRect out = ClampWindowToWorkArea(window, avail);
    EXPECT_EQ(out, window);
}

} // namespace
} // namespace exosnap::ui
