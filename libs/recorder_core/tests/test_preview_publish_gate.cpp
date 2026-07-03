#include <gtest/gtest.h>

#include "preview_publish_gate.h"

// Pure state-machine tests for the throttle logic that decides when a
// composed video frame should be surfaced to the live preview callback
// (Strand 3 slice 1). No clock, no D3D11, no threads -- deterministic.
//
// IMPORTANT: the cadence tests below feed the gate the EXACT PTS values the
// production CFR scheduler produces -- pts = frame_idx * frame_interval_ns
// where frame_interval_ns = (10'000'000 / fps) * 100 is TRUNCATED
// (16'666'600 ns at 60 fps, 33'333'300 ns at 30 fps). An earlier version of
// these tests used the rounded-up 16'666'667 ns and thereby missed a real
// bug: a 33'333'333 ns gate threshold rejects two truncated 60 fps intervals
// (33'333'200 ns) and halves the preview rate to ~20 Hz.

using recorder_core::kPreviewMinIntervalNs;
using recorder_core::PreviewPublishGate;

namespace {

constexpr uint64_t kNsPerMs = 1'000'000ULL;

// Production CFR frame interval derivation (must match video_thread.cpp):
// truncated 100ns ticks, then scaled to ns.
constexpr uint64_t CfrFrameIntervalNs(uint64_t fps) {
    return (10'000'000ULL / fps) * 100ULL;
}

static_assert(CfrFrameIntervalNs(60) == 16'666'600ULL, "60 fps CFR interval must match video_thread truncation");
static_assert(CfrFrameIntervalNs(30) == 33'333'300ULL, "30 fps CFR interval must match video_thread truncation");

// Runs `fps` production-exact CFR ticks over `seconds` and returns how many
// the gate publishes.
int CountPublishes(PreviewPublishGate& gate, uint64_t fps, uint64_t seconds) {
    const uint64_t interval = CfrFrameIntervalNs(fps);
    int published = 0;
    for (uint64_t tick = 0; tick < fps * seconds; ++tick) {
        if (gate.ShouldPublish(tick * interval)) {
            ++published;
        }
    }
    return published;
}

} // namespace

TEST(PreviewPublishGate, FirstFramePublishesImmediately) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    EXPECT_TRUE(gate.ShouldPublish(0));
}

TEST(PreviewPublishGate, ThrottlesInsideTheWindow) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    ASSERT_TRUE(gate.ShouldPublish(0));

    // Real frames arriving inside the throttle window must not publish.
    EXPECT_FALSE(gate.ShouldPublish(10 * kNsPerMs));
    EXPECT_FALSE(gate.ShouldPublish(20 * kNsPerMs));

    // Once the interval has elapsed, the next real frame publishes again.
    EXPECT_TRUE(gate.ShouldPublish(31 * kNsPerMs));
}

TEST(PreviewPublishGate, SixtyFpsCfrPtsYieldsApprox30Hz) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    const int published = CountPublishes(gate, 60, 2);
    // Two 60 fps intervals = 33'333'200 ns >= 30 ms -> every 2nd frame,
    // i.e. ~30 Hz. The old 33'333'333 ns threshold rejected that span and
    // produced ~20 publishes/s here (every 3rd frame).
    EXPECT_GE(published, 58); // >= 29 Hz over 2 s
    EXPECT_LE(published, 62); // <= 31 Hz over 2 s
}

TEST(PreviewPublishGate, ThirtyFpsCfrPtsYieldsApprox30Hz) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    const int published = CountPublishes(gate, 30, 2);
    // One 30 fps interval = 33'333'300 ns >= 30 ms -> every frame publishes.
    // The old threshold rejected every other frame (-> ~15 Hz).
    EXPECT_GE(published, 58);
    EXPECT_LE(published, 62);
}

TEST(PreviewPublishGate, HundredTwentyFpsCfrPtsStaysNear30Hz) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    const int published = CountPublishes(gate, 120, 2);
    // Four 120 fps intervals = 33'333'200 ns >= 30 ms -> every 4th frame.
    EXPECT_GE(published, 58);
    EXPECT_LE(published, 62);
}

TEST(PreviewPublishGate, NeverExceedsGateRate) {
    // Degenerate high-rate input: 1000 candidate frames 1 ms apart must not
    // publish more often than once per 30 ms window.
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    int published = 0;
    for (uint64_t tick = 0; tick < 1000; ++tick) {
        if (gate.ShouldPublish(tick * kNsPerMs)) {
            ++published;
        }
    }
    EXPECT_GE(published, 33); // ~1 s / 30 ms
    EXPECT_LE(published, 34);
}

TEST(PreviewPublishGate, NonMonotonicTimestampIsIgnoredNotUnderflowed) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    ASSERT_TRUE(gate.ShouldPublish(100 * kNsPerMs));
    // A timestamp earlier than the last published one must not publish and
    // must not corrupt internal state (defends against clock jitter/misuse).
    EXPECT_FALSE(gate.ShouldPublish(50 * kNsPerMs));
    // Normal forward progress still works afterwards.
    EXPECT_TRUE(gate.ShouldPublish(200 * kNsPerMs));
}

TEST(PreviewPublishGate, ResetAllowsImmediatePublishAgain) {
    PreviewPublishGate gate(kPreviewMinIntervalNs);
    ASSERT_TRUE(gate.ShouldPublish(0));
    EXPECT_FALSE(gate.ShouldPublish(1 * kNsPerMs));

    gate.Reset();
    EXPECT_TRUE(gate.ShouldPublish(2 * kNsPerMs));
}

TEST(PreviewPublishGate, ZeroIntervalPublishesEveryFrame) {
    PreviewPublishGate gate(0);
    EXPECT_TRUE(gate.ShouldPublish(0));
    EXPECT_TRUE(gate.ShouldPublish(1));
    EXPECT_TRUE(gate.ShouldPublish(2));
}
