#include <gtest/gtest.h>

#include "services/PushedSourceState.h"

// State-machine tests for the DXGI preview's pushed-source switch-over — the
// decisions the renderer makes when handing the preview from its own WGC capture
// to the engine's shared source during recording, and back on stop. Pure logic;
// the live GPU round-trip is exercised by the engine WARP test and inherently
// live paths.

using exosnap::PushedSourceState;

TEST(PushedSourceState, IdlePreviewPollsWgcAndDrawsOwnOverlay) {
    PushedSourceState s;
    EXPECT_FALSE(s.active);
    EXPECT_TRUE(s.PollsWgc());               // preview runs its own WGC capture
    EXPECT_FALSE(s.DrawsPushedBackground()); // nothing pushed yet
    EXPECT_TRUE(s.DrawsWebcamOverlay());     // renderer composites its own PiP
    EXPECT_FALSE(s.ShouldStopWgcGraph());    // no reason to stop the capture
}

TEST(PushedSourceState, RecordingOpensSourceAndStopsWgcOnce) {
    PushedSourceState s;
    s.OnSourceOpened(); // engine shared handle adopted on the render device

    EXPECT_TRUE(s.active);
    EXPECT_FALSE(s.PollsWgc());          // stop polling the second capture
    EXPECT_TRUE(s.ShouldStopWgcGraph()); // ... and tear the WGC graph down, once

    s.OnWgcGraphStopped();
    EXPECT_FALSE(s.ShouldStopWgcGraph()); // idempotent: never stops twice
}

TEST(PushedSourceState, CountdownHoldsLastWgcImageUntilFirstPushedFrame) {
    PushedSourceState s;
    s.OnSourceOpened();
    s.OnWgcGraphStopped();

    // Active but no engine frame yet: hold the last WGC image (no black flash) and
    // keep drawing the renderer's own overlay so the PiP does not blink out.
    EXPECT_FALSE(s.DrawsPushedBackground());
    EXPECT_TRUE(s.DrawsWebcamOverlay());

    s.OnFrameConsumed(); // first engine frame arrives
    EXPECT_TRUE(s.DrawsPushedBackground());
    // The pushed frame already contains the PiP -> suppress the renderer's overlay.
    EXPECT_FALSE(s.DrawsWebcamOverlay());
}

TEST(PushedSourceState, StopRevertsToWgcAndRedrawsOwnOverlay) {
    PushedSourceState s;
    s.OnSourceOpened();
    s.OnWgcGraphStopped();
    s.OnFrameConsumed();
    ASSERT_TRUE(s.DrawsPushedBackground());

    s.Reset(); // recording stopped / resources released
    EXPECT_FALSE(s.active);
    EXPECT_TRUE(s.PollsWgc()); // preview restarts its own WGC capture
    EXPECT_FALSE(s.DrawsPushedBackground());
    EXPECT_TRUE(s.DrawsWebcamOverlay()); // renderer draws its own PiP again
    EXPECT_FALSE(s.ShouldStopWgcGraph());
}

TEST(PushedSourceState, SessionRestartReArmsWgcStopAndClearsFrame) {
    PushedSourceState s;
    s.OnSourceOpened();
    s.OnWgcGraphStopped();
    s.OnFrameConsumed();
    s.Reset();

    // A fresh recording (new shared handle) must stop the WGC graph again and start
    // from no-frame (hold) rather than inheriting the previous session's has_frame.
    s.OnSourceOpened();
    EXPECT_TRUE(s.ShouldStopWgcGraph());
    EXPECT_FALSE(s.DrawsPushedBackground());
    EXPECT_TRUE(s.DrawsWebcamOverlay());
}
