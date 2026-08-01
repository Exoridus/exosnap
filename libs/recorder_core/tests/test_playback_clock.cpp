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

using recorder_core::kDefaultMaxVideoQueueBytes;
using recorder_core::kMinVideoQueueFrames;

// Decoded frames are BGRA: 4 bytes per pixel, whatever the source chroma was.
constexpr size_t BgraFrameBytes(size_t w, size_t h) {
    return w * h * 4u;
}
constexpr size_t k1080p = BgraFrameBytes(1920, 1080); //  ~7.9 MB
constexpr size_t k1440p = BgraFrameBytes(2560, 1440); // ~14.7 MB
constexpr size_t k2160p = BgraFrameBytes(3840, 2160); // ~33.2 MB

// A frame size small enough that the byte budget can never be the binding
// constraint -- for the tests that are only about the rate-derived count.
constexpr size_t kTinyFrame = 1024;

TEST(VideoQueueCapacityForFrameRate, SixtyFpsMatchesTheOriginalHandComputedSize) {
    // 60 x 0.2 s = 12 frames in flight, + 1/3 headroom = 16 -- the value the
    // queue was hand-sized to before it became rate-derived.
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
}

TEST(VideoQueueCapacityForFrameRate, HighFrameRateClipGetsAQueueThatCanHoldItsDecodeAhead) {
    // The whole point: at 144 fps the decode-ahead window holds ~29 frames, so
    // a fixed capacity of 16 would drop frames before the clock reached them.
    const size_t capacity = VideoQueueCapacityForFrameRate(144.0, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes);
    EXPECT_GE(capacity, static_cast<size_t>(144.0 * kDecodeAhead));
    EXPECT_EQ(capacity, 38u);
}

TEST(VideoQueueCapacityForFrameRate, LowFrameRateClipKeepsTheFloor) {
    EXPECT_EQ(VideoQueueCapacityForFrameRate(15.0, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(30.0, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
}

TEST(VideoQueueCapacityForFrameRate, UnknownOrAbsurdRateFallsBackToTheFloor) {
    EXPECT_EQ(VideoQueueCapacityForFrameRate(0.0, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(-1.0, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(1e9, kDecodeAhead, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, 0.0, kTinyFrame, kDefaultMaxVideoQueueBytes), 16u);
}

// --- Byte budget -----------------------------------------------------------
//
// The rate-derived count alone is a frame COUNT, and decoded frames are BGRA:
// the same 0.2 s window is 235 MB at 1440p60 but over a gigabyte at 2160p120.
// Since the producer blocks only once the queue is full, that depth is the
// steady state, not a peak.

TEST(VideoQueueCapacityForFrameRate, CommonResolutionsAreNotCappedByTheByteBudget) {
    // 1080p and 1440p at ordinary rates stay on the rate-derived value: the
    // budget must not quietly shrink the decode-ahead window for normal clips.
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, kDecodeAhead, k1080p, kDefaultMaxVideoQueueBytes), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, kDecodeAhead, k1440p, kDefaultMaxVideoQueueBytes), 16u);
    EXPECT_EQ(VideoQueueCapacityForFrameRate(144.0, kDecodeAhead, k1080p, kDefaultMaxVideoQueueBytes), 38u);
}

TEST(VideoQueueCapacityForFrameRate, HighResolutionHighRateClipStaysWithinTheByteBudget) {
    // 2160p120: the rate alone asks for 32 frames = ~1.06 GB resident.
    const size_t capacity = VideoQueueCapacityForFrameRate(120.0, kDecodeAhead, k2160p, kDefaultMaxVideoQueueBytes);
    EXPECT_LT(capacity, 32u);
    EXPECT_LE(capacity * k2160p, kDefaultMaxVideoQueueBytes);
}

TEST(VideoQueueCapacityForFrameRate, MisdeclaredFrameRateCannotAllocateGigabytes) {
    // A Matroska file with a millisecond timebase routinely declares
    // r_frame_rate = 1000/1. That is 200 frames in the window -- 2.9 GB at
    // 1440p -- and the old absurd-rate guard only tripped above 500000 fps.
    const size_t capacity = VideoQueueCapacityForFrameRate(1000.0, kDecodeAhead, k1440p, kDefaultMaxVideoQueueBytes);
    EXPECT_LE(capacity * k1440p, kDefaultMaxVideoQueueBytes);
}

TEST(VideoQueueCapacityForFrameRate, KeepsAUsableMinimumEvenWhenTheBudgetCannotCoverIt) {
    // The budget must never starve the pipeline down to nothing: below a few
    // frames there is no decode-ahead left at all, so the minimum wins over
    // the budget rather than the other way round.
    EXPECT_EQ(VideoQueueCapacityForFrameRate(60.0, kDecodeAhead, k2160p, 1024u), kMinVideoQueueFrames);
    EXPECT_GE(
        VideoQueueCapacityForFrameRate(120.0, kDecodeAhead, BgraFrameBytes(7680, 4320), kDefaultMaxVideoQueueBytes),
        kMinVideoQueueFrames);
}

TEST(VideoQueueCapacityForFrameRate, UnknownFrameSizeFallsBackToTheRateDerivedCount) {
    // Frame size is not known until the first frame is decoded; a zero must
    // mean "no byte information" rather than "budget for zero frames".
    EXPECT_EQ(VideoQueueCapacityForFrameRate(144.0, kDecodeAhead, 0u, kDefaultMaxVideoQueueBytes), 38u);
}

// --- Audio preroll trim ----------------------------------------------------
//
// A playback seek lands on the KEYFRAME at or before start_us, so both streams
// start decoding earlier than asked. Video discards the frames in between
// (ShouldConvertDecodedFrame), but audio has no equivalent: every decoded
// sample from the keyframe onwards used to go straight into the ring while the
// clock was seeded to start_us. That is a fixed offset for the whole run, up
// to the keyframe interval -- 2 s at the product default.

using recorder_core::AudioPrerollFramesToDrop;

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
