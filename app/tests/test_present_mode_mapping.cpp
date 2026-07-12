#include "diagnostics/PresentAccumulator.h"
#include "diagnostics/PresentModeMapping.h"
#include <gtest/gtest.h>

using namespace exosnap::diagnostics;

TEST(PresentModeMapping, InvalidEventIsUnavailable) {
    RawPresentEvent ev{};
    ev.valid = false;
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.mode, PresentMode::Unknown);
}

TEST(PresentModeMapping, ComposedFlipMapsToComposed) {
    RawPresentEvent ev{/*present_mode_code=*/4, /*sync_interval=*/1,
                       /*tearing_flag=*/false, /*interval_ms=*/16.6, /*valid=*/true};
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_TRUE(s.available);
    EXPECT_EQ(s.mode, PresentMode::Composed);
    EXPECT_FALSE(s.tearing);
    EXPECT_DOUBLE_EQ(s.present_interval_ms, 16.6);
}

TEST(PresentModeMapping, IndependentFlipMapsThrough) {
    RawPresentEvent ev{3, 1, false, 8.3, true}; // Hardware_Independent_Flip
    EXPECT_EQ(MapPresentEvent(ev).mode, PresentMode::IndependentFlip);
}

TEST(PresentModeMapping, HardwareComposedIndependentFlipIsIndependentFlip) {
    RawPresentEvent ev{8, 1, false, 8.3, true}; // Hardware_Composed_Independent_Flip
    EXPECT_EQ(MapPresentEvent(ev).mode, PresentMode::IndependentFlip);
}

TEST(PresentModeMapping, LegacyFlipMapsToExclusiveFullscreen) {
    RawPresentEvent ev{1, 0, true, 6.9, true};
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_EQ(s.mode, PresentMode::ExclusiveFullscreen);
    EXPECT_TRUE(s.tearing); // sync_interval 0 + tearing flag
}

TEST(PresentModeMapping, SyncIntervalZeroImpliesTearing) {
    RawPresentEvent ev{3, /*sync_interval=*/0, /*tearing_flag=*/false, 7.0, true};
    EXPECT_TRUE(MapPresentEvent(ev).tearing); // interval 0 = uncapped/tearing-capable
}

// ── PresentAccumulator (per-recording aggregates) ──────────────────────────────

TEST(PresentAccumulatorTest, ObserveCountsPresentsDiscardsAndFlips) {
    PresentAccumulator acc;
    acc.Observe(PresentMode::Composed, /*discarded=*/false);        // present 1
    acc.Observe(PresentMode::Composed, /*discarded=*/true);         // present 2, discard 1
    acc.Observe(PresentMode::IndependentFlip, /*discarded=*/false); // present 3, flip 1
    acc.Observe(PresentMode::IndependentFlip, /*discarded=*/false); // present 4, no flip
    acc.Observe(PresentMode::Composed, /*discarded=*/false);        // present 5, flip 2
    EXPECT_EQ(acc.present_count, 5u);
    EXPECT_EQ(acc.discarded_count, 1u);
    EXPECT_EQ(acc.mode_flip_count, 2u);
}

TEST(PresentAccumulatorTest, UnknownModeNeitherFlipsNorLatches) {
    PresentAccumulator acc;
    acc.Observe(PresentMode::Composed, false);
    acc.Observe(PresentMode::Unknown, false);  // must not count as a flip
    acc.Observe(PresentMode::Composed, false); // still Composed -> no flip
    EXPECT_EQ(acc.present_count, 3u);
    EXPECT_EQ(acc.mode_flip_count, 0u);
}

// The core per-recording guarantee: statistics from an earlier recording (and the
// idle desktop between recordings) must never accumulate into the next recording.
// PresentMonEtwSession::SetTargetProcessId drives this Reset() at every attribution
// boundary; here we prove the pure accounting it relies on.
TEST(PresentAccumulatorTest, ResetDropsPriorRecordingTotals) {
    PresentAccumulator acc;

    // Recording 1: a heavy, discard-laden, mode-flipping session.
    for (int i = 0; i < 400; ++i)
        acc.Observe(PresentMode::Composed, /*discarded=*/true);
    for (int i = 0; i < 10; ++i)
        acc.Observe(i % 2 == 0 ? PresentMode::IndependentFlip : PresentMode::Composed, false);
    ASSERT_GT(acc.present_count, 400u);
    ASSERT_GT(acc.discarded_count, 0u);
    ASSERT_GE(acc.mode_flip_count, 5u);

    // Attribution boundary (recording stop -> idle -> recording 2 start).
    acc.Reset();
    EXPECT_EQ(acc.present_count, 0u);
    EXPECT_EQ(acc.discarded_count, 0u);
    EXPECT_EQ(acc.mode_flip_count, 0u);
    EXPECT_EQ(acc.last_mode, PresentMode::Unknown);

    // Recording 2: a clean session. Its stats must reflect ONLY recording 2, so the
    // discard ratio stays at 0 (no dilution) and the flip counter stays below the
    // rec.present.modeflip threshold (5) — no false latch from recording 1.
    for (int i = 0; i < 300; ++i)
        acc.Observe(PresentMode::Composed, /*discarded=*/false);
    EXPECT_EQ(acc.present_count, 300u);
    EXPECT_EQ(acc.discarded_count, 0u);
    EXPECT_EQ(acc.mode_flip_count, 0u);
}
