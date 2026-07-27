// A/V epoch alignment (av_epoch_align.h): where an audio track starts relative
// to the video track, and what that does to its packet timestamps.

#include <gtest/gtest.h>

#include "av_epoch_align.h"

#include <cstdint>

namespace {

using recorder_core::AudioTimelineShiftNs;
using recorder_core::kMaxAudioEpochShiftNs;
using recorder_core::ShiftAudioPts;

// 100 ns units per millisecond.
constexpr uint64_t kMs100ns = 10000ULL;
constexpr int64_t kMsNs = 1000000LL;

TEST(AudioTimelineShift, IdenticalEpochsNeedNoShift) {
    EXPECT_EQ(AudioTimelineShiftNs(1000 * kMs100ns, 1000 * kMs100ns), 0);
}

TEST(AudioTimelineShift, AudioStartingAfterVideoIsPushedLater) {
    // The audio endpoint came up 80 ms after the video epoch: the track has to
    // start 80 ms into the file, not at 0 (which is what makes audio lead
    // picture when the real start is assumed instead of measured).
    EXPECT_EQ(AudioTimelineShiftNs(1080 * kMs100ns, 1000 * kMs100ns), 80 * kMsNs);
}

TEST(AudioTimelineShift, AudioStartingBeforeVideoIsTrimmed) {
    EXPECT_EQ(AudioTimelineShiftNs(1000 * kMs100ns, 1120 * kMs100ns), -120 * kMsNs);
}

TEST(AudioTimelineShift, AbsurdEpochDifferenceIsClamped) {
    // A bogus timestamp must not push audio minutes away from the picture.
    EXPECT_EQ(AudioTimelineShiftNs(600000 * kMs100ns, 0), kMaxAudioEpochShiftNs);
    EXPECT_EQ(AudioTimelineShiftNs(0, 600000 * kMs100ns), -kMaxAudioEpochShiftNs);
}

TEST(ShiftAudioPts, PositiveShiftMovesPacketsLater) {
    uint64_t out = 0;
    ASSERT_TRUE(ShiftAudioPts(0, 80 * kMsNs, out));
    EXPECT_EQ(out, static_cast<uint64_t>(80 * kMsNs));
    ASSERT_TRUE(ShiftAudioPts(1000 * static_cast<uint64_t>(kMsNs), 80 * kMsNs, out));
    EXPECT_EQ(out, static_cast<uint64_t>(1080 * kMsNs));
}

TEST(ShiftAudioPts, NegativeShiftTrimsTheHeadAndKeepsTheRest) {
    uint64_t out = 0;
    // 120 ms of audio predates the first video frame: dropped.
    EXPECT_FALSE(ShiftAudioPts(0, -120 * kMsNs, out));
    EXPECT_FALSE(ShiftAudioPts(119 * static_cast<uint64_t>(kMsNs), -120 * kMsNs, out));
    // Everything from the video epoch on is kept, rebased to 0.
    ASSERT_TRUE(ShiftAudioPts(120 * static_cast<uint64_t>(kMsNs), -120 * kMsNs, out));
    EXPECT_EQ(out, 0u);
    ASSERT_TRUE(ShiftAudioPts(200 * static_cast<uint64_t>(kMsNs), -120 * kMsNs, out));
    EXPECT_EQ(out, static_cast<uint64_t>(80 * kMsNs));
}

TEST(ShiftAudioPts, ZeroShiftIsAPassthrough) {
    uint64_t out = 0;
    ASSERT_TRUE(ShiftAudioPts(12345, 0, out));
    EXPECT_EQ(out, 12345u);
}

} // namespace
