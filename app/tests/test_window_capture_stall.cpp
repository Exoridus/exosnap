// Mid-session window-capture stall predicates (S4, pure): the Stage-1 gate that
// decides whether to gather facts at all, and the Stage-2 cause classifier that
// only speaks with positive evidence.

#include "diagnostics/WindowCaptureStall.h"

#include <gtest/gtest.h>

using namespace exosnap::diagnostics;

namespace {

WindowStallSnapshot starvedWindow() {
    WindowStallSnapshot s;
    s.is_window_target = true;
    s.actual_fps = 0.0;
    s.frames_duplicated_rising = true;
    s.seconds_starved = 12.0;
    return s;
}

WindowTargetFacts fullscreenShaped() {
    WindowTargetFacts f;
    f.valid = true;
    f.visible = true;
    f.window_rect = RECT{0, 0, 1920, 1080};
    f.monitor_rect = RECT{0, 0, 1920, 1080};
    f.style = WS_POPUP | WS_VISIBLE;
    return f;
}

// ---- Stage 1: WindowStallSuspected ----

TEST(WindowStallGate, StarvedWindowTargetIsSuspected) {
    EXPECT_TRUE(WindowStallSuspected(starvedWindow()));
}

TEST(WindowStallGate, MonitorTargetNeverSuspected) {
    WindowStallSnapshot s = starvedWindow();
    s.is_window_target = false;
    EXPECT_FALSE(WindowStallSuspected(s));
}

TEST(WindowStallGate, ProducingFramesNotSuspected) {
    WindowStallSnapshot s = starvedWindow();
    s.actual_fps = 59.9;
    EXPECT_FALSE(WindowStallSuspected(s));
}

TEST(WindowStallGate, NoRisingDuplicatesNotSuspected) {
    // A source can legitimately have 0 fps without the encoder padding (paused).
    WindowStallSnapshot s = starvedWindow();
    s.frames_duplicated_rising = false;
    EXPECT_FALSE(WindowStallSuspected(s));
}

TEST(WindowStallGate, UnderTenSecondsNotSuspected) {
    WindowStallSnapshot s = starvedWindow();
    s.seconds_starved = 9.0;
    EXPECT_FALSE(WindowStallSuspected(s));
}

TEST(WindowStallGate, AtTenSecondsSuspected) {
    WindowStallSnapshot s = starvedWindow();
    s.seconds_starved = 10.0;
    EXPECT_TRUE(WindowStallSuspected(s));
}

// ---- Stage 2: EvaluateWindowStall ----

TEST(WindowStallCauseTest, MinimizedIsMinimized) {
    WindowTargetFacts f = fullscreenShaped();
    f.minimized = true;
    EXPECT_EQ(EvaluateWindowStall(f, false), WindowStallCause::Minimized);
}

TEST(WindowStallCauseTest, FullscreenShapedWithQunsIsExclusive) {
    WindowTargetFacts f = fullscreenShaped();
    f.quns_d3d_fullscreen = true;
    EXPECT_EQ(EvaluateWindowStall(f, false), WindowStallCause::ExclusiveFullscreen);
}

TEST(WindowStallCauseTest, FullscreenShapedWithPresentFseIsExclusive) {
    EXPECT_EQ(EvaluateWindowStall(fullscreenShaped(), /*present_fse=*/true), WindowStallCause::ExclusiveFullscreen);
}

TEST(WindowStallCauseTest, FullscreenShapedWithoutSignalStaysSilent) {
    // Fullscreen-shaped but neither QUNS nor PresentMon corroborates: a
    // legitimately static borderless window must not raise a notification.
    EXPECT_EQ(EvaluateWindowStall(fullscreenShaped(), false), WindowStallCause::None);
}

TEST(WindowStallCauseTest, NormalWindowStaysSilent) {
    WindowTargetFacts f = fullscreenShaped();
    f.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE; // captioned -> Normal shape
    f.quns_d3d_fullscreen = true;               // even with a stray signal
    EXPECT_EQ(EvaluateWindowStall(f, true), WindowStallCause::None);
}

} // namespace
