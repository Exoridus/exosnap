// Mid-recording window-capture stall (QCR-804, pure): the frame-progress monitor
// that owns the timing and latching contract, and the Stage-2 classifier that
// only speaks when the silence is not legitimate.
//
// Every timing assertion here is driven by the elapsed_seconds the samples carry,
// never by a wall clock — no test in this file sleeps.

#include "diagnostics/WindowCaptureStall.h"

#include <gtest/gtest.h>

using namespace exosnap::diagnostics;
using exosnap::engine::DiagnosticsLifecycle;

namespace {

// The diagnostics publish cadence the monitor is actually driven at: ~5 Hz.
// Time is counted in whole ticks throughout so no assertion depends on floating
// point accumulating exactly over hundreds of iterations.
constexpr double kTickSeconds = 0.2;
constexpr int kStarveTicks = 50; // kStallStarveSeconds / kTickSeconds

// A healthy recording sample for a WGC window target at tick `tick`.
WindowStallSample recordingSample(uint64_t generation, uint64_t frames_captured, int tick) {
    WindowStallSample s;
    s.session_generation = generation;
    s.is_window_target = true;
    s.capture_expected = true;
    s.frames_captured = frames_captured;
    s.elapsed_seconds = tick * kTickSeconds;
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

WindowTargetFacts normalWindow() {
    WindowTargetFacts f = fullscreenShaped();
    f.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE; // captioned -> Normal shape
    return f;
}

// Drives the monitor over the inclusive tick range with a frozen frame counter,
// answering every Starved with `verdict`. Returns how many stalls were signalled.
int runStarved(WindowCaptureStallMonitor& monitor, uint64_t generation, uint64_t frames, int from_tick, int to_tick,
               WindowStallVerdict verdict = WindowStallVerdict::Stalled) {
    int stalls = 0;
    for (int tick = from_tick; tick <= to_tick; ++tick) {
        if (monitor.Observe(recordingSample(generation, frames, tick)) == WindowStallSignal::Starved) {
            ++stalls;
            monitor.ApplyVerdict(verdict);
        }
    }
    return stalls;
}

// ---- The threshold constant --------------------------------------------------

TEST(WindowStallThreshold, MatchesTheTickBudgetTheTestsAssume) {
    EXPECT_DOUBLE_EQ(kStallStarveSeconds, kStarveTicks * kTickSeconds);
}

// ---- CaptureProgressExpected -------------------------------------------------

TEST(CaptureProgressExpectedTest, OnlyRecordingExpectsFrames) {
    EXPECT_TRUE(CaptureProgressExpected(DiagnosticsLifecycle::Recording));
    for (const auto lifecycle :
         {DiagnosticsLifecycle::Idle, DiagnosticsLifecycle::Initializing, DiagnosticsLifecycle::Paused,
          DiagnosticsLifecycle::Stopping, DiagnosticsLifecycle::Completed, DiagnosticsLifecycle::Failed}) {
        EXPECT_FALSE(CaptureProgressExpected(lifecycle));
    }
}

// ---- Stage 1: normal operation -----------------------------------------------

TEST(WindowStallMonitor, ProducingFramesNeverStalls) {
    WindowCaptureStallMonitor monitor;
    uint64_t frames = 0;
    for (int tick = 0; tick <= 300; ++tick) { // 60 s
        frames += 12;                         // 60 fps over a 200 ms publish window
        EXPECT_EQ(monitor.Observe(recordingSample(1, frames, tick)), WindowStallSignal::None);
    }
    EXPECT_FALSE(monitor.reported_stall());
    EXPECT_EQ(monitor.reported_episodes(), 0u);
}

TEST(WindowStallMonitor, StaticContentIsNotAStall) {
    // The picture never changes, but WGC keeps handing frames over — which is
    // exactly what frames_captured measures. Nothing must be reported. Detection
    // is frame progress, deliberately never image difference.
    WindowCaptureStallMonitor monitor;
    uint64_t frames = 0;
    for (int tick = 0; tick <= 600; ++tick) { // 120 s
        frames += 1;                          // slow but real progress
        EXPECT_EQ(monitor.Observe(recordingSample(1, frames, tick)), WindowStallSignal::None);
    }
    EXPECT_EQ(monitor.reported_episodes(), 0u);
}

TEST(WindowStallMonitor, MonitorCaptureNeverEntersTheWindowStallPath) {
    WindowCaptureStallMonitor monitor;
    for (int tick = 0; tick <= 300; ++tick) {
        WindowStallSample s = recordingSample(1, 100, tick); // frozen counter
        s.is_window_target = false;                          // display / region capture
        EXPECT_EQ(monitor.Observe(s), WindowStallSignal::None);
    }
    EXPECT_FALSE(monitor.reported_stall());
}

// ---- Stage 1: the confirmed stall --------------------------------------------

TEST(WindowStallMonitor, StarvationBelowThresholdIsSilent) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    EXPECT_EQ(runStarved(monitor, 1, 100, 1, kStarveTicks - 1), 0);
}

TEST(WindowStallMonitor, StarvationAtThresholdIsSignalledExactlyOnce) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    EXPECT_EQ(runStarved(monitor, 1, 100, 1, kStarveTicks), 1);
    EXPECT_TRUE(monitor.reported_stall());
    EXPECT_EQ(monitor.reported_episodes(), 1u);
    EXPECT_GE(monitor.seconds_without_progress(), kStallStarveSeconds);
}

TEST(WindowStallMonitor, ContinuedStallNeverRepeatsTheSignal) {
    // Five more minutes of nothing must not produce a second notification.
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, kStarveTicks), 1);
    EXPECT_EQ(runStarved(monitor, 1, 100, kStarveTicks + 1, kStarveTicks + 1500), 0);
    EXPECT_EQ(monitor.reported_episodes(), 1u);
}

TEST(WindowStallMonitor, StarvationWithoutAVerdictIsNotAskedAgain) {
    // A caller that never answers must not turn the signal into a per-tick loop.
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    int stalls = 0;
    for (int tick = 1; tick <= 300; ++tick) {
        if (monitor.Observe(recordingSample(1, 100, tick)) == WindowStallSignal::Starved)
            ++stalls; // deliberately no ApplyVerdict
    }
    EXPECT_EQ(stalls, 1);
}

TEST(WindowStallMonitor, SuppressedStallIsNotReRaisedWhileStarvationContinues) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, kStarveTicks, WindowStallVerdict::Legitimate), 1);
    EXPECT_FALSE(monitor.reported_stall());
    EXPECT_EQ(monitor.reported_episodes(), 0u); // suppressed episodes are not counted
    EXPECT_EQ(runStarved(monitor, 1, 100, kStarveTicks + 1, kStarveTicks + 600, WindowStallVerdict::Legitimate), 0);
}

// ---- Stage 1: recovery -------------------------------------------------------

TEST(WindowStallMonitor, FramesResumingRecoversExactlyOnce) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, 60), 1);

    EXPECT_EQ(monitor.Observe(recordingSample(1, 101, 61)), WindowStallSignal::Recovered);
    EXPECT_FALSE(monitor.reported_stall());
    // And not again on the next healthy sample.
    EXPECT_EQ(monitor.Observe(recordingSample(1, 113, 62)), WindowStallSignal::None);
}

TEST(WindowStallMonitor, ASuppressedStallRecoversSilently) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, 60, WindowStallVerdict::Unknown), 1);
    // Nothing was reported, so nothing may be cleared.
    EXPECT_EQ(monitor.Observe(recordingSample(1, 101, 61)), WindowStallSignal::None);
}

TEST(WindowStallMonitor, ASecondIndependentStallIsReportedAgain) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, 60), 1);
    ASSERT_EQ(monitor.Observe(recordingSample(1, 101, 61)), WindowStallSignal::Recovered);

    EXPECT_EQ(runStarved(monitor, 1, 101, 62, 150), 1);
    EXPECT_TRUE(monitor.reported_stall());
    EXPECT_EQ(monitor.reported_episodes(), 2u);
}

// ---- Stage 1: pause ----------------------------------------------------------

TEST(WindowStallMonitor, PauseDoesNotAccumulateStarveTime) {
    // Ten minutes paused: the capture legitimately produces nothing and the
    // starve clock must not run.
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    constexpr int kResume = 3000; // 600 s
    for (int tick = 1; tick <= kResume; ++tick) {
        WindowStallSample s = recordingSample(1, 100, tick);
        s.capture_expected = false; // DiagnosticsLifecycle::Paused
        ASSERT_EQ(monitor.Observe(s), WindowStallSignal::None);
    }
    EXPECT_FALSE(monitor.reported_stall());
    // Resuming starts the clock from the resume point, not from the pause point.
    EXPECT_EQ(runStarved(monitor, 1, 100, kResume + 1, kResume + kStarveTicks - 1), 0);
    EXPECT_EQ(runStarved(monitor, 1, 100, kResume + kStarveTicks, kResume + kStarveTicks + 5), 1);
}

TEST(WindowStallMonitor, PauseDoesNotClearAStallThatIsStillStanding) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, 60), 1);
    WindowStallSample paused = recordingSample(1, 100, 61);
    paused.capture_expected = false;
    EXPECT_EQ(monitor.Observe(paused), WindowStallSignal::None);
    EXPECT_TRUE(monitor.reported_stall());
}

// ---- Stage 1: session end and session switch ---------------------------------

TEST(WindowStallMonitor, SessionEndProducesNoSignal) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    for (int tick = 1; tick <= 300; ++tick) {
        WindowStallSample s = recordingSample(1, 100, tick);
        s.capture_expected = false; // Stopping / Completed / Failed
        ASSERT_EQ(monitor.Observe(s), WindowStallSignal::None);
    }
}

TEST(WindowStallMonitor, ANewSessionNeverInheritsTheOldOnesStall) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, 60), 1);
    ASSERT_TRUE(monitor.reported_stall());

    // Generation 2 == a new recording (also how a target switch reaches here).
    EXPECT_EQ(monitor.Observe(recordingSample(2, 0, 0)), WindowStallSignal::None);
    EXPECT_FALSE(monitor.reported_stall());
    EXPECT_EQ(monitor.reported_episodes(), 0u);
    // No Recovered from the old session leaks into the new one either.
    EXPECT_EQ(monitor.Observe(recordingSample(2, 12, 1)), WindowStallSignal::None);
}

TEST(WindowStallMonitor, TargetEvidenceDoesNotCrossSessions) {
    // Frame counts restart at 0 for the new target; the old target's much higher
    // count must not read as "progress" nor as instant starvation.
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(7, 900000, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 7, 900000, 1, 60), 1);
    ASSERT_EQ(monitor.Observe(recordingSample(8, 0, 0)), WindowStallSignal::None);
    EXPECT_EQ(runStarved(monitor, 8, 0, 1, kStarveTicks - 1), 0);
}

TEST(WindowStallMonitor, ExplicitResetForgetsEverything) {
    WindowCaptureStallMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 100, 0)), WindowStallSignal::None);
    ASSERT_EQ(runStarved(monitor, 1, 100, 1, 60), 1);
    monitor.Reset();
    EXPECT_FALSE(monitor.reported_stall());
    EXPECT_EQ(monitor.reported_episodes(), 0u);
    EXPECT_EQ(monitor.Observe(recordingSample(1, 100, 61)), WindowStallSignal::None);
}

// ---- Stage 2: ClassifyConfirmedStall -----------------------------------------

TEST(WindowStallVerdictTest, FullscreenShapedIsReported) {
    EXPECT_EQ(ClassifyConfirmedStall(fullscreenShaped(), /*present_fse=*/false), WindowStallVerdict::Stalled);
}

TEST(WindowStallVerdictTest, MinimizedIsLegitimate) {
    WindowTargetFacts f = fullscreenShaped();
    f.minimized = true;
    EXPECT_EQ(ClassifyConfirmedStall(f, false), WindowStallVerdict::Legitimate);
}

TEST(WindowStallVerdictTest, CloakedIsLegitimate) {
    WindowTargetFacts f = fullscreenShaped();
    f.cloaked = true; // another virtual desktop / suspended UWP
    EXPECT_EQ(ClassifyConfirmedStall(f, false), WindowStallVerdict::Legitimate);
}

TEST(WindowStallVerdictTest, HiddenOrDeadWindowIsLegitimate) {
    WindowTargetFacts hidden = fullscreenShaped();
    hidden.visible = false;
    EXPECT_EQ(ClassifyConfirmedStall(hidden, false), WindowStallVerdict::Legitimate);

    const WindowTargetFacts dead; // valid == false
    EXPECT_EQ(ClassifyConfirmedStall(dead, false), WindowStallVerdict::Legitimate);
}

TEST(WindowStallVerdictTest, OrdinaryWindowStaysSilent) {
    // A captioned window that stopped drawing is indistinguishable from one that
    // simply has nothing to draw. ExoSnap does not guess.
    EXPECT_EQ(ClassifyConfirmedStall(normalWindow(), false), WindowStallVerdict::Unknown);
    EXPECT_EQ(ClassifyConfirmedStall(normalWindow(), /*present_fse=*/true), WindowStallVerdict::Unknown);
}

// ---- Stage 2: cause refinement (the FSE hint only) ---------------------------

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

TEST(WindowStallCauseTest, FullscreenShapedWithoutSignalClaimsNoCause) {
    // Still reported as a stall (see WindowStallVerdictTest above) — but the
    // notification must not name exclusive fullscreen without corroboration.
    EXPECT_EQ(EvaluateWindowStall(fullscreenShaped(), false), WindowStallCause::None);
    EXPECT_EQ(ClassifyConfirmedStall(fullscreenShaped(), false), WindowStallVerdict::Stalled);
}

TEST(WindowStallCauseTest, NormalWindowClaimsNoCause) {
    WindowTargetFacts f = normalWindow();
    f.quns_d3d_fullscreen = true; // even with a stray signal
    EXPECT_EQ(EvaluateWindowStall(f, true), WindowStallCause::None);
}

} // namespace
