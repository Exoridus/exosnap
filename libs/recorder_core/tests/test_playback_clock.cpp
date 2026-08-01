#include <gtest/gtest.h>

#include "playback_clock.h"

#include <vector>

namespace {

using recorder_core::AudioClockMs;
using recorder_core::ClockPositionToFrames;
using recorder_core::InterpolateClockPosition;
using recorder_core::SelectFrameForClock;
using recorder_core::VideoQueueCapacityForFrameRate;

// The audio ring is 9600 frames at 48 kHz = 200 ms, which is exactly how far
// ahead of the clock the shared decode thread may race.
constexpr double kDecodeAhead = 0.2;

TEST(VideoQueueCapacityForFrameRate, SixtyFpsMatchesTheOriginalHandComputedSize) {
    // 60 x 0.2 s = 12 frames in flight, + 1/3 headroom = 16 -- the value the
    // queue was hand-sized to before it became rate-derived.
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, kDecodeAhead), 16u);
}

TEST(VideoQueueCapacityForFrameRate, HighFrameRateClipGetsAQueueThatCanHoldItsDecodeAhead) {
    // The whole point: at 144 fps the decode-ahead window holds ~29 frames, so
    // a fixed capacity of 16 would drop frames before the clock reached them.
    const size_t capacity = VideoQueueCapacityForFrameRate(144.0, kDecodeAhead);
    EXPECT_GE(capacity, static_cast<size_t>(144.0 * kDecodeAhead));
    EXPECT_EQ(capacity, 38u);
}

TEST(VideoQueueCapacityForFrameRate, LowFrameRateClipKeepsTheFloor) {
    EXPECT_EQ(VideoQueueCapacityForFrameRate(15.0, kDecodeAhead), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(30.0, kDecodeAhead), 16u);
}

TEST(VideoQueueCapacityForFrameRate, UnknownOrAbsurdRateFallsBackToTheFloor) {
    EXPECT_EQ(VideoQueueCapacityForFrameRate(0.0, kDecodeAhead), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(-1.0, kDecodeAhead), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(1e9, kDecodeAhead), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, 0.0), 16u);
}

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

// --- IAudioClock position conversion ---------------------------------------
// The play cursor's unit is whatever IAudioClock::GetFrequency reports: bytes
// per second for a shared-mode stream, frames per second for exclusive mode.
// The conversion must be right in both cases without being told which it is.

TEST(ClockPositionToFrames, SharedModeByteUnits) {
    // 48 kHz stereo float32: 8 bytes per frame -> 384000 bytes/s.
    constexpr uint64_t kFrequency = 384000;
    EXPECT_EQ(ClockPositionToFrames(0, kFrequency, 48000), 0u);
    EXPECT_EQ(ClockPositionToFrames(384000, kFrequency, 48000), 48000u); // 1 s
    EXPECT_EQ(ClockPositionToFrames(192000, kFrequency, 48000), 24000u); // 0.5 s
}

TEST(ClockPositionToFrames, ExclusiveModeFrameUnits) {
    EXPECT_EQ(ClockPositionToFrames(48000, 48000, 48000), 48000u);
    EXPECT_EQ(ClockPositionToFrames(22050, 44100, 44100), 22050u);
}

TEST(ClockPositionToFrames, DegenerateInputsAreZeroNotDivByZero) {
    EXPECT_EQ(ClockPositionToFrames(1000, 0, 48000), 0u);
    EXPECT_EQ(ClockPositionToFrames(1000, 48000, 0), 0u);
}

TEST(ClockPositionToFrames, LongClipDoesNotOverflow) {
    // 10 hours of a shared-mode byte position: the naive position * rate would
    // wrap; divide-first must not.
    constexpr uint64_t kFrequency = 384000;
    constexpr uint64_t kTenHoursSeconds = 36000;
    EXPECT_EQ(ClockPositionToFrames(kFrequency * kTenHoursSeconds, kFrequency, 48000), 48000ull * kTenHoursSeconds);
}

TEST(InterpolateClockPosition, AdvancesThePositionByTheQpcDelta) {
    constexpr uint64_t kFrequency = 384000;        // bytes/s
    constexpr uint64_t kTenMs100ns = 100000;       // 10 ms
    constexpr uint64_t kMaxExtrapolation = 200000; // 20 ms
    // A reading taken 10 ms ago is 10 ms of stream units behind now.
    const uint64_t out =
        InterpolateClockPosition(384000, kFrequency, 1000000, 1000000 + kTenMs100ns, kMaxExtrapolation);
    EXPECT_EQ(out, 384000u + 3840u);
}

TEST(InterpolateClockPosition, ExtrapolationIsClamped) {
    constexpr uint64_t kFrequency = 384000;
    constexpr uint64_t kMaxExtrapolation = 200000; // 20 ms
    // A five-second-old reading must advance the clock by 20 ms, not 5 s.
    const uint64_t out = InterpolateClockPosition(0, kFrequency, 1000000, 1000000 + 50000000, kMaxExtrapolation);
    EXPECT_EQ(out, 7680u); // 20 ms of 384000 bytes/s
}

TEST(InterpolateClockPosition, NeverRunsBackwardsOrOnAMissingTimestamp) {
    constexpr uint64_t kFrequency = 384000;
    EXPECT_EQ(InterpolateClockPosition(5000, kFrequency, 0, 1000000, 200000), 5000u);       // no qpc pair
    EXPECT_EQ(InterpolateClockPosition(5000, kFrequency, 2000000, 1000000, 200000), 5000u); // now is older
    EXPECT_EQ(InterpolateClockPosition(5000, 0, 1000000, 2000000, 200000), 5000u);          // no frequency
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
    EXPECT_EQ(sel.dropped_count, 1u); // pts 100 dropped -- dropped_count is purely positional,
                                      // per FrameSelection::dropped_count's own doc comment
}

TEST(SelectFrameForClock, ClockPastAllFramesSelectsLastAndDropsRest) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 999);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 2u);
    EXPECT_EQ(sel.dropped_count, 2u);
}

} // namespace
