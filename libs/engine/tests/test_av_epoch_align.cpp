// A/V epoch alignment (av_epoch_align.h): where an audio track starts relative
// to the video track, and what that does to its packet timestamps.

#include <gtest/gtest.h>

#include "av_epoch_align.h"

#include <cstdint>

namespace {

using exosnap::engine::AudioEpochNsFromPacket;
using exosnap::engine::AudioTimelineShiftNs;
using exosnap::engine::IsPlausibleAudioEpoch;
using exosnap::engine::kMaxAudioEpochShiftNs;
using exosnap::engine::ShiftAudioPts;

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

// VFR video epoch clamping (VfrVideoEpoch): the PTS origin must never precede
// the session start. VFR PTS is `frame ticks - epoch`, with a negative delta
// clamped to 0 by the caller — these tests derive PTS the same way.

using exosnap::engine::ClampedVfrVideoEpochTicks100ns;

constexpr uint64_t kSessionStart100ns = 50000 * kMs100ns; // 50 s of machine uptime

int64_t VfrPtsMs(int64_t frameTicks100ns, int64_t epochTicks100ns) {
    int64_t delta = frameTicks100ns - epochTicks100ns;
    if (delta < 0) {
        delta = 0;
    }
    return delta / static_cast<int64_t>(kMs100ns);
}

TEST(VfrVideoEpoch, FirstFrameBeforeSessionStartIsClampedToSessionStart) {
    // Static desktop: the last real present was 5 s before Record() was called.
    const int64_t staleFrame = static_cast<int64_t>(kSessionStart100ns) - 5000 * static_cast<int64_t>(kMs100ns);
    EXPECT_EQ(ClampedVfrVideoEpochTicks100ns(staleFrame, kSessionStart100ns), static_cast<int64_t>(kSessionStart100ns));
}

TEST(VfrVideoEpoch, FirstFrameExactlyAtSessionStartIsTheEpoch) {
    EXPECT_EQ(ClampedVfrVideoEpochTicks100ns(static_cast<int64_t>(kSessionStart100ns), kSessionStart100ns),
              static_cast<int64_t>(kSessionStart100ns));
}

TEST(VfrVideoEpoch, FirstFrameAfterSessionStartIsUnchanged) {
    const int64_t frame = static_cast<int64_t>(kSessionStart100ns) + 200 * static_cast<int64_t>(kMs100ns);
    EXPECT_EQ(ClampedVfrVideoEpochTicks100ns(frame, kSessionStart100ns), frame);
}

TEST(VfrVideoEpoch, InvalidTimestampsFallBackToSessionStart) {
    EXPECT_EQ(ClampedVfrVideoEpochTicks100ns(0, kSessionStart100ns), static_cast<int64_t>(kSessionStart100ns));
    EXPECT_EQ(ClampedVfrVideoEpochTicks100ns(-1, kSessionStart100ns), static_cast<int64_t>(kSessionStart100ns));
    EXPECT_EQ(ClampedVfrVideoEpochTicks100ns(INT64_MIN, kSessionStart100ns), static_cast<int64_t>(kSessionStart100ns));
}

TEST(VfrVideoEpoch, StaleFirstFrameDoesNotInflateTheTimeline) {
    // The regression: desktop static for 5 s, first frame carries the stale
    // present time, second frame arrives 100 ms after recording start. With an
    // unclamped origin the second frame lands at 5100 ms and the file claims
    // 5 s of footage that never happened; with the clamp it lands at 100 ms.
    const int64_t staleFrame = static_cast<int64_t>(kSessionStart100ns) - 5000 * static_cast<int64_t>(kMs100ns);
    const int64_t epoch = ClampedVfrVideoEpochTicks100ns(staleFrame, kSessionStart100ns);
    EXPECT_EQ(VfrPtsMs(staleFrame, epoch), 0); // first frame: negative delta clamps to PTS 0
    const int64_t second = static_cast<int64_t>(kSessionStart100ns) + 100 * static_cast<int64_t>(kMs100ns);
    EXPECT_EQ(VfrPtsMs(second, epoch), 100);
}

TEST(VfrVideoEpoch, FollowUpFramesStayMonotone) {
    // First frame is the stale pre-session one (negative delta, PTS 0), then
    // strictly increasing post-session frames — the whole sequence must come
    // out non-negative and non-decreasing from the clamped origin alone.
    const int64_t staleFrame = static_cast<int64_t>(kSessionStart100ns) - 3000 * static_cast<int64_t>(kMs100ns);
    const int64_t epoch = ClampedVfrVideoEpochTicks100ns(staleFrame, kSessionStart100ns);
    int64_t lastPtsMs = -1;
    EXPECT_EQ(VfrPtsMs(staleFrame, epoch), 0);
    for (int i = 1; i <= 5; ++i) {
        const int64_t frame = static_cast<int64_t>(kSessionStart100ns) + (i * 17) * static_cast<int64_t>(kMs100ns);
        const int64_t ptsMs = VfrPtsMs(frame, epoch);
        EXPECT_GT(ptsMs, lastPtsMs);
        EXPECT_GE(ptsMs, 0);
        lastPtsMs = ptsMs;
    }
}

TEST(VfrVideoEpoch, PauseBeforeFirstFrameAdvancesTheFloor) {
    // Pause served before the epoch exists: the caller advances the floor by
    // the paused span. A stale first frame then clamps to the advanced floor,
    // so the pause is excluded from the timeline instead of becoming a
    // lead-in; the first post-resume frame lands at/near PTS 0.
    const uint64_t paused = 2000 * kMs100ns;
    const uint64_t floor100ns = kSessionStart100ns + paused;
    const int64_t staleFrame = static_cast<int64_t>(kSessionStart100ns) - 1000 * static_cast<int64_t>(kMs100ns);
    const int64_t epoch = ClampedVfrVideoEpochTicks100ns(staleFrame, floor100ns);
    EXPECT_EQ(epoch, static_cast<int64_t>(floor100ns));
    const int64_t firstAfterResume = static_cast<int64_t>(floor100ns) + 16 * static_cast<int64_t>(kMs100ns);
    EXPECT_EQ(VfrPtsMs(firstAfterResume, epoch), 16);
}

} // namespace
