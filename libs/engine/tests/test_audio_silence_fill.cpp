// Wall-clock silence-fill policy (audio_silence_fill.h): the arithmetic behind
// holding an audio timeline together while its source delivers nothing.

#include <gtest/gtest.h>

#include "audio_silence_fill.h"

#include <cstdint>
#include <limits>

namespace {

using exosnap::engine::IsSilentStall;
using exosnap::engine::kSilentStallThresholdNs;
using exosnap::engine::RemainingGapFrames;
using exosnap::engine::SilenceFillFrames;

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kNoBound = std::numeric_limits<uint64_t>::max();

TEST(SilenceFillFrames, ConvertsElapsedWallClockToFrames) {
    EXPECT_EQ(SilenceFillFrames(1000 * kMs, 0, 48000, kNoBound), 48000u);
    EXPECT_EQ(SilenceFillFrames(500 * kMs, 0, 48000, kNoBound), 24000u);
    EXPECT_EQ(SilenceFillFrames(1500 * kMs, 500 * kMs, 44100, kNoBound), 44100u);
}

TEST(SilenceFillFrames, ClockThatHasNotMovedFillsNothing) {
    EXPECT_EQ(SilenceFillFrames(1000 * kMs, 1000 * kMs, 48000, kNoBound), 0u);
    // A clock that went backwards (never expected, but must not underflow into
    // a colossal fill).
    EXPECT_EQ(SilenceFillFrames(900 * kMs, 1000 * kMs, 48000, kNoBound), 0u);
}

TEST(SilenceFillFrames, ZeroSampleRateFillsNothing) {
    EXPECT_EQ(SilenceFillFrames(1000 * kMs, 0, 0, kNoBound), 0u);
}

TEST(SilenceFillFrames, BoundsOneCallSoALongStallIsFilledIncrementally) {
    // 60 s of stall, bounded to 5 s per call.
    constexpr uint64_t kMax = 48000ULL * 5;
    EXPECT_EQ(SilenceFillFrames(60000 * kMs, 0, 48000, kMax), kMax);
}

TEST(IsSilentStall, OrdinaryPacketCadenceIsNotAStall) {
    // 10 ms and 100 ms between packets: normal, nothing to fill.
    EXPECT_FALSE(IsSilentStall(10 * kMs, 0));
    EXPECT_FALSE(IsSilentStall(100 * kMs, 0));
    EXPECT_FALSE(IsSilentStall(299 * kMs, 0));
}

TEST(IsSilentStall, QuietPastTheThresholdIsAStall) {
    EXPECT_TRUE(IsSilentStall(kSilentStallThresholdNs, 0));
    EXPECT_TRUE(IsSilentStall(2000 * kMs, 0));
    // Measured against the anchor, not against zero.
    EXPECT_FALSE(IsSilentStall(2000 * kMs, 1900 * kMs));
    EXPECT_TRUE(IsSilentStall(2000 * kMs, 1000 * kMs));
}

TEST(IsSilentStall, ClockNotMovingIsNeverAStall) {
    EXPECT_FALSE(IsSilentStall(1000 * kMs, 1000 * kMs));
    EXPECT_FALSE(IsSilentStall(900 * kMs, 1000 * kMs));
}

TEST(RemainingGapFrames, UnfilledGapIsHonoredInFull) {
    EXPECT_EQ(RemainingGapFrames(4800, 0), 4800u);
}

TEST(RemainingGapFrames, GapAlreadyCoveredByWallClockSilenceIsNotFilledTwice) {
    // The same 100 ms outage reported by both the wall clock and the device
    // position must land on the timeline once, not twice.
    EXPECT_EQ(RemainingGapFrames(4800, 4800), 0u);
    EXPECT_EQ(RemainingGapFrames(4800, 9600), 0u);
}

TEST(RemainingGapFrames, PartialWallClockCoverageLeavesTheRemainder) {
    EXPECT_EQ(RemainingGapFrames(4800, 4000), 800u);
}

} // namespace
