// A/V epoch alignment (av_epoch_align.h): where an audio track starts relative
// to the video track, and what that does to its packet timestamps.

#include <gtest/gtest.h>

#include "av_epoch_align.h"

#include <cstdint>

namespace {

using recorder_core::AudioEpochNsFromPacket;
using recorder_core::AudioTimelineShiftNs;
using recorder_core::IsPlausibleAudioEpoch;
using recorder_core::kMaxAudioEpochShiftNs;
using recorder_core::ShiftAudioPts;

// 100 ns units per millisecond.
constexpr uint64_t kMs100ns = 10000ULL;
constexpr int64_t kMsNs = 1000000LL;

constexpr uint64_t kMsNsU = 1000000ULL;

TEST(AudioEpochFromPacket, WalksBackOverTheFramesAlreadyOnTheTimeline) {
    // A packet timestamped at 5 s that lands after 2 s of already-fed audio
    // puts the timeline's zero point at 3 s.
    EXPECT_EQ(AudioEpochNsFromPacket(5000 * kMsNsU, 96000, 48000), 3000 * kMsNsU);
    // The first packet of a recording: nothing in front of it, so its own
    // timestamp IS the zero point.
    EXPECT_EQ(AudioEpochNsFromPacket(5000 * kMsNsU, 0, 48000), 5000 * kMsNsU);
}

TEST(AudioEpochFromPacket, CountsOnlyFramesActuallyFed) {
    // The caller resolved a 1 s reported gap down to 0 (the wall clock had
    // already filled that stretch). Counting the REPORTED gap instead of the
    // fed one would walk the zero point a whole second too far back, and the
    // muxer would trim a second of real audio off the head of the track.
    constexpr uint64_t kPacketQpc = 5000 * kMsNsU;
    constexpr uint64_t kFramesOnTimeline = 48000; // 1 s already fed
    constexpr uint64_t kReportedGapFrames = 48000;
    EXPECT_EQ(AudioEpochNsFromPacket(kPacketQpc, kFramesOnTimeline, 48000), 4000 * kMsNsU);
    EXPECT_NE(AudioEpochNsFromPacket(kPacketQpc, kFramesOnTimeline + kReportedGapFrames, 48000), 4000 * kMsNsU);
}

TEST(AudioEpochFromPacket, UnmeasurableInputsReportZero) {
    EXPECT_EQ(AudioEpochNsFromPacket(1000, 0, 0), 0u);         // no sample rate
    EXPECT_EQ(AudioEpochNsFromPacket(1000, 48000, 48000), 0u); // walk-back precedes the origin
}

TEST(IsPlausibleAudioEpoch, AnUnmeasuredEpochIsNotUsable) {
    EXPECT_FALSE(IsPlausibleAudioEpoch(0, 1000 * kMs100ns));
}

TEST(IsPlausibleAudioEpoch, EverydayOffsetsAreUsable) {
    EXPECT_TRUE(IsPlausibleAudioEpoch(1080 * kMs100ns, 1000 * kMs100ns));
    EXPECT_TRUE(IsPlausibleAudioEpoch(1000 * kMs100ns, 1120 * kMs100ns));
}

TEST(IsPlausibleAudioEpoch, ReadingsBeyondTheEnvelopeAreRejectedNotClamped) {
    // The caller must fall back to the session baseline for these; applying
    // the clamped shift would still be seconds wrong.
    EXPECT_FALSE(IsPlausibleAudioEpoch(600000 * kMs100ns, 1000 * kMs100ns));
    EXPECT_FALSE(IsPlausibleAudioEpoch(1000 * kMs100ns, 600000 * kMs100ns));
}

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
