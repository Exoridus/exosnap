#include <gtest/gtest.h>

#include "playback_clock.h"

#include <vector>

namespace {

using recorder_core::AudioClockMs;
using recorder_core::SelectFrameForClock;

TEST(AudioClockMs, ZeroFramesIsZero) {
    EXPECT_EQ(AudioClockMs(0, 48000), 0);
}

TEST(AudioClockMs, OneSecondAt48k) {
    EXPECT_EQ(AudioClockMs(48000, 48000), 1000);
}

TEST(AudioClockMs, HalfSecondAt44_1k) {
    // 22050 frames / 44100 Hz = 0.5s = 500ms.
    EXPECT_EQ(AudioClockMs(22050, 44100), 500);
}

TEST(AudioClockMs, ZeroSampleRateIsZeroNotDivByZero) {
    EXPECT_EQ(AudioClockMs(48000, 0), 0);
}

TEST(SelectFrameForClock, EmptyQueueSelectsNothing) {
    std::vector<int64_t> pts_ms;
    const auto sel = SelectFrameForClock(pts_ms, 1000);
    EXPECT_FALSE(sel.index.has_value());
    EXPECT_EQ(sel.dropped_count, 0u);
}

TEST(SelectFrameForClock, ClockBeforeFirstFrameSelectsNothingYet) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 50);
    EXPECT_FALSE(sel.index.has_value());
    EXPECT_EQ(sel.dropped_count, 0u);
}

TEST(SelectFrameForClock, PicksLatestFrameAtOrBeforeClock) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 250);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 1u);        // pts 200
    EXPECT_EQ(sel.dropped_count, 1u); // pts 100 dropped
}

TEST(SelectFrameForClock, ExactMatchSelectsThatFrame) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 200);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 1u);
    EXPECT_EQ(sel.dropped_count, 0u);
}

TEST(SelectFrameForClock, ClockPastAllFramesSelectsLastAndDropsRest) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 999);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 2u);
    EXPECT_EQ(sel.dropped_count, 2u);
}

} // namespace
