#include "recorder_core/frame_pacing.h"
#include <gtest/gtest.h>
#include <span>
using namespace recorder_core;

TEST(PacingRingSize, AdaptiveFromRatio) {
    EXPECT_EQ(ComputePacingRingSize(144, 60), 5u); // ceil(144/60)=3, +2 = 5
    EXPECT_EQ(ComputePacingRingSize(240, 60), 6u); // ceil(240/60)=4, +2 = 6
    EXPECT_EQ(ComputePacingRingSize(60, 60), 4u);  // ceil=1, +2=3 -> clamped up to min 4
}
TEST(PacingRingSize, ClampsToMax) {
    EXPECT_EQ(ComputePacingRingSize(1000, 60), 12u); // huge ratio clamps to 12
}
TEST(PacingRingSize, UnknownRefreshFallsBackTo8) {
    EXPECT_EQ(ComputePacingRingSize(0, 60), 8u);
    EXPECT_EQ(ComputePacingRingSize(144, 0), 8u);
}

TEST(SelectFrame, SmoothPicksNearestFresh) {
    const uint64_t ring[] = {100, 200, 300, 400}; // present QPCs, ascending
    // slot=250, last_emitted=0 → nearest is 200 (idx1) or 300 (idx2); |250-200|=|250-300| → tie→lower idx
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 250, 0, FramePacingMode::Smooth);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 1u);         // 200 chosen
    EXPECT_EQ(d.newly_dropped, 1u); // entry 100 skipped (fresh, older than chosen)
}
TEST(SelectFrame, NewestPicksLastFresh) {
    const uint64_t ring[] = {100, 200, 300};
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 150, 0, FramePacingMode::Newest);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 2u);         // newest (300)
    EXPECT_EQ(d.newly_dropped, 2u); // 100 and 200 skipped
}
TEST(SelectFrame, FiltersAlreadyEmitted) {
    const uint64_t ring[] = {100, 200, 300};
    // last_emitted=200 → only 300 is fresh
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 250, 200, FramePacingMode::Smooth);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 2u);
    EXPECT_EQ(d.newly_dropped, 0u);
}
TEST(SelectFrame, NoFreshEntriesDuplicates) {
    const uint64_t ring[] = {100, 200};
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 999, 200, FramePacingMode::Smooth);
    EXPECT_FALSE(d.emit); // all <= last_emitted → duplicate
    EXPECT_EQ(d.newly_dropped, 0u);
}
TEST(SelectFrame, EmptyRingDuplicates) {
    auto d = SelectFrameForSlot(std::span<const uint64_t>(), 100, 0, FramePacingMode::Smooth);
    EXPECT_FALSE(d.emit);
}
