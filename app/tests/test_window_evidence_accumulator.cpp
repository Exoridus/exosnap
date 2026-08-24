// The pure evidence-accumulation core behind WindowEvidenceProbe (S2a): from a
// stream of (hub frame kind, generation, shape) samples it derives the timing
// facts the severity ladder consumes. No Qt, no COM — time is injected.

#include "diagnostics/WindowEvidenceAccumulator.h"

#include <gtest/gtest.h>

using namespace exosnap::diagnostics;
using exosnap::engine::HubFrameKind;

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point at(double seconds) {
    return Clock::time_point{} + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

TEST(EvidenceAccumulator, NoneAccumulatesSubscribedTime) {
    WindowEvidenceAccumulator acc;
    acc.Reset(at(0));
    acc.Update(at(0.1), HubFrameKind::None, 0, WindowShape::FullscreenShaped);
    const WindowHubEvidence e = acc.Evidence(at(2.5));
    EXPECT_EQ(e.kind, HubFrameKind::None);
    EXPECT_GE(e.seconds_subscribed, 2.4);
    // Never produced: "since fresh" tracks the subscription age.
    EXPECT_GE(e.seconds_since_fresh_frame, 2.4);
    EXPECT_FALSE(e.fresh_frame_since_fullscreen_shape);
}

TEST(EvidenceAccumulator, ProducingThenStoppingHoldsAndAges) {
    WindowEvidenceAccumulator acc;
    acc.Reset(at(0));
    // Producing: fresh frames arrive while windowed (Normal shape).
    acc.Update(at(0.1), HubFrameKind::Live, 1, WindowShape::Normal);
    acc.Update(at(0.2), HubFrameKind::Live, 2, WindowShape::Normal);
    // Source stops: hub now holds the last good frame, generation frozen.
    acc.Update(at(0.3), HubFrameKind::Held, 2, WindowShape::Normal);
    const WindowHubEvidence e = acc.Evidence(at(3.3));
    EXPECT_EQ(e.kind, HubFrameKind::Held);
    // ~3 s since the last fresh frame at t=0.2.
    EXPECT_GE(e.seconds_since_fresh_frame, 3.0);
    EXPECT_LT(e.seconds_since_fresh_frame, 3.2);
}

TEST(EvidenceAccumulator, FreshFrameAfterFullscreenTransitionArmsCorrelation) {
    WindowEvidenceAccumulator acc;
    acc.Reset(at(0));
    // Windowed and producing.
    acc.Update(at(0.1), HubFrameKind::Live, 1, WindowShape::Normal);
    // Transition to fullscreen-shaped; correlation resets.
    acc.Update(at(1.0), HubFrameKind::Live, 1, WindowShape::FullscreenShaped);
    EXPECT_FALSE(acc.Evidence(at(1.0)).fresh_frame_since_fullscreen_shape);
    // A genuinely new frame arrives AFTER the transition (paused-video case).
    acc.Update(at(1.5), HubFrameKind::Held, 2, WindowShape::FullscreenShaped);
    EXPECT_TRUE(acc.Evidence(at(2.0)).fresh_frame_since_fullscreen_shape);
}

TEST(EvidenceAccumulator, FrozenAtTransitionKeepsCorrelationFalse) {
    WindowEvidenceAccumulator acc;
    acc.Reset(at(0));
    acc.Update(at(0.1), HubFrameKind::Live, 5, WindowShape::Normal);
    // Switch to fullscreen (FSE) and never produce another frame.
    acc.Update(at(1.0), HubFrameKind::Held, 5, WindowShape::FullscreenShaped);
    acc.Update(at(4.0), HubFrameKind::Held, 5, WindowShape::FullscreenShaped);
    const WindowHubEvidence e = acc.Evidence(at(4.0));
    EXPECT_FALSE(e.fresh_frame_since_fullscreen_shape);
    EXPECT_GE(e.seconds_since_fresh_frame, 3.8); // frozen since t=0.1
}

TEST(EvidenceAccumulator, ResetForgetsHistory) {
    WindowEvidenceAccumulator acc;
    acc.Reset(at(0));
    acc.Update(at(1.0), HubFrameKind::Live, 3, WindowShape::FullscreenShaped);
    acc.Reset(at(10.0)); // new subscription
    const WindowHubEvidence e = acc.Evidence(at(11.0));
    EXPECT_EQ(e.kind, HubFrameKind::None);
    EXPECT_NEAR(e.seconds_subscribed, 1.0, 0.05);
    EXPECT_FALSE(e.fresh_frame_since_fullscreen_shape);
}

} // namespace
