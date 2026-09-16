// test_duration_skew.cpp — what the A/V duration skew is allowed to mean.
//
// The metric compared the video end against the AUDIO ENCODER's last PTS. The
// muxer shifts every audio track onto the video timeline before writing it, so
// those two are on different timelines and differ by exactly that shift: a
// correctly aligned recording whose audio clock started 200 ms after the video
// clock reported 200 ms of drift, session after session, with nothing wrong.
//
// The fix must not be a correction that subtracts the offset, because that would
// also erase a real 200 ms loss. Both cases are here, and the second is what
// proves the first is not just arithmetic.

#include <exosnap/engine/session_stats.h>

#include <gtest/gtest.h>

using exosnap::engine::ComputeDurationSkew;
using exosnap::engine::DurationSkew;

namespace {

constexpr uint64_t kMs = 1'000'000ULL;

// A 30 s recording: video ends at 30 s on the video timeline.
constexpr uint64_t kVideoEnd = 30'000 * kMs;

TEST(DurationSkew, AnAlignedRecordingWithAnEpochOffsetReportsNoSkew) {
    // The defect, as the reviewer's case: the audio clock started 200 ms after
    // the video clock, the muxer shifted the track by -200 ms, and the file ends
    // aligned. The encoder's own last PTS is 30.2 s -- which is what the metric
    // used to read, reporting 200 ms.
    const std::array<uint64_t, 3> aligned{kVideoEnd, 0, 0};
    const DurationSkew skew = ComputeDurationSkew(kVideoEnd, aligned);
    ASSERT_TRUE(skew.available);
    EXPECT_NEAR(skew.ms, 0.0, 0.001) << "an aligned file must not report its epoch offset as drift";
}

TEST(DurationSkew, ARealLossOfTwoHundredMillisecondsStillReportsIt) {
    // The control. If the fix were "subtract the epoch offset", this would read
    // 0 too, and the metric would be worth nothing.
    const std::array<uint64_t, 3> aligned{kVideoEnd - 200 * kMs, 0, 0};
    const DurationSkew skew = ComputeDurationSkew(kVideoEnd, aligned);
    ASSERT_TRUE(skew.available);
    EXPECT_NEAR(skew.ms, 200.0, 0.001) << "audio that really ends early must still be reported";
}

TEST(DurationSkew, AudioEndingLateIsReportedToo) {
    // Sign-independent: the question is distance from the picture, either way.
    const std::array<uint64_t, 3> aligned{kVideoEnd + 200 * kMs, 0, 0};
    const DurationSkew skew = ComputeDurationSkew(kVideoEnd, aligned);
    ASSERT_TRUE(skew.available);
    EXPECT_NEAR(skew.ms, 200.0, 0.001);
}

TEST(DurationSkew, NothingToCompareIsUnavailableRatherThanZero) {
    // "No skew" and "nothing was measured" must not be the same value: a caller
    // that cannot tell them apart publishes a zero it never measured.
    EXPECT_FALSE(ComputeDurationSkew(0, {kVideoEnd, 0, 0}).available) << "no video: nothing to compare";
    EXPECT_FALSE(ComputeDurationSkew(kVideoEnd, {0, 0, 0}).available) << "no audio written: nothing to compare";
    EXPECT_FALSE(ComputeDurationSkew(0, {0, 0, 0}).available);
}

TEST(DurationSkew, ATrackWithNothingWrittenIsNotAZeroSkew) {
    // A second track that never produced a packet has an aligned end of 0. Read
    // as a duration, that is 30 s of skew; read correctly, it is not a
    // measurement at all. The written track is what the answer comes from.
    const std::array<uint64_t, 3> aligned{kVideoEnd - 50 * kMs, 0, 0};
    const DurationSkew skew = ComputeDurationSkew(kVideoEnd, aligned);
    ASSERT_TRUE(skew.available);
    EXPECT_NEAR(skew.ms, 50.0, 0.001) << "an absent track must not be read as a 30 s skew";
}

TEST(DurationSkew, TheWorstTrackIsWhatIsReported) {
    // Multi-track: the question is whether ANY audio ends away from the picture,
    // so an average would hide one bad track behind two good ones.
    const std::array<uint64_t, 3> aligned{kVideoEnd, kVideoEnd - 300 * kMs, kVideoEnd - 10 * kMs};
    const DurationSkew skew = ComputeDurationSkew(kVideoEnd, aligned);
    ASSERT_TRUE(skew.available);
    EXPECT_NEAR(skew.ms, 300.0, 0.001);
}

TEST(DurationSkew, TheOldComputationIsWhatThisReplaces) {
    // The premise, stated so it cannot quietly stop being true: comparing the
    // video end against an audio end on the CAPTURE timeline reports the epoch
    // offset. This is the arithmetic the metric used to do.
    const uint64_t encoder_last_pts = kVideoEnd + 200 * kMs; // capture timeline
    const double old_metric = static_cast<double>(encoder_last_pts - kVideoEnd) / 1e6;
    EXPECT_NEAR(old_metric, 200.0, 0.001);

    // And the same recording, measured where the file actually ends.
    const DurationSkew skew = ComputeDurationSkew(kVideoEnd, {kVideoEnd, 0, 0});
    ASSERT_TRUE(skew.available);
    EXPECT_LT(skew.ms, old_metric);
}

} // namespace
