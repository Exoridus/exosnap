#include <gtest/gtest.h>

#include "playback_clock.h"

#include <vector>

namespace {

using exosnap::engine::AudioClockMs;
using exosnap::engine::ClockPositionToFrames;
using exosnap::engine::InterpolateClockPosition;
using exosnap::engine::SelectFrameForClock;

// --- Audio preroll trim ----------------------------------------------------
//
// A playback seek lands on the KEYFRAME at or before start_us, so both streams
// start decoding earlier than asked. Video discards the frames in between
// (ShouldConvertDecodedFrame), but audio has no equivalent: every decoded
// sample from the keyframe onwards used to go straight into the ring while the
// clock was seeded to start_us. That is a fixed offset for the whole run, up
// to the keyframe interval -- 2 s at the product default.

using exosnap::engine::AudioPrerollFramesToDrop;

constexpr uint32_t k48k = 48000;

TEST(AudioPrerollFramesToDrop, BlockEntirelyAtOrAfterTheStartIsKeptWhole) {
    EXPECT_EQ(AudioPrerollFramesToDrop(/*block_pts_us=*/1'000'000, /*block_frames=*/480, k48k,
                                       /*start_us=*/1'000'000),
              0u);
    EXPECT_EQ(AudioPrerollFramesToDrop(2'000'000, 480, k48k, 1'000'000), 0u);
}

TEST(AudioPrerollFramesToDrop, BlockEntirelyBeforeTheStartIsDroppedWhole) {
    // 480 frames at 48 kHz is 10 ms, so this block ends at 1.010 s -- all of it
    // is preroll for a start at 1.5 s.
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 480, k48k, 1'500'000), 480u);
}

TEST(AudioPrerollFramesToDrop, BlockEndingExactlyOnTheStartIsDroppedWhole) {
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 480, k48k, 1'010'000), 480u);
}

TEST(AudioPrerollFramesToDrop, BlockStraddlingTheStartIsTrimmedSampleAccurately) {
    // Block covers 1.000 s .. 1.020 s (960 frames); playback starts 5 ms in,
    // which is exactly 240 frames at 48 kHz.
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 960, k48k, 1'005'000), 240u);
}

TEST(AudioPrerollFramesToDrop, TrimRoundsToTheNearestSampleRatherThanTruncating) {
    // 1 us short of a whole sample boundary must not leave a sample of the
    // past in the ring, nor eat one that belongs to the future.
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 960, k48k, 1'005'001), 240u);
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 960, k48k, 1'004'999), 240u);
}

TEST(AudioPrerollFramesToDrop, UnknownSampleRateOrEmptyBlockTrimsNothing) {
    // Without a rate the offset cannot be converted to samples; keeping the
    // audio (slightly offset) beats replacing it with silence.
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 960, 0, 1'500'000), 0u);
    EXPECT_EQ(AudioPrerollFramesToDrop(1'000'000, 0, k48k, 1'500'000), 0u);
}

TEST(AudioPrerollFramesToDrop, NeverReportsMoreFramesThanTheBlockHolds) {
    // A wildly early block must saturate at its own length, never overrun it.
    EXPECT_EQ(AudioPrerollFramesToDrop(0, 960, k48k, 60'000'000), 960u);
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
