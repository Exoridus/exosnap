// The recording clock, and what it does about a pause.
//
// `elapsed_seconds` is what the user reads as "how long is my recording". A
// paused capture writes no frames, so a clock that counted through a pause
// promised a file longer than the one that lands -- and the tray tooltip, the
// Record page and the finished result all read the same number.

#include "session_stats_collector.h"

#include <gtest/gtest.h>

using exosnap::engine::CapturedSeconds;
using namespace std::chrono_literals;

namespace {

constexpr std::chrono::nanoseconds kNone = std::chrono::nanoseconds::zero();

} // namespace

TEST(CapturedSeconds, AnUninterruptedRecordingIsItsWallClock) {
    EXPECT_DOUBLE_EQ(CapturedSeconds(10s, kNone, kNone), 10.0);
}

TEST(CapturedSeconds, PausedTimeIsNotRecordedTime) {
    // Ten seconds of wall clock with four spent paused is a six-second file.
    EXPECT_DOUBLE_EQ(CapturedSeconds(10s, 4s, kNone), 6.0);
}

TEST(CapturedSeconds, APauseThatIsStillOpenHoldsTheClockStill) {
    // The clock must stand still WHILE the capture is held. Counting an open
    // pause as recorded time and only subtracting it on resume would make the
    // number jump backwards in front of the user.
    EXPECT_DOUBLE_EQ(CapturedSeconds(10s, kNone, 3s), 7.0);
    EXPECT_DOUBLE_EQ(CapturedSeconds(12s, kNone, 5s), 7.0) << "the clock moved during a pause";
}

TEST(CapturedSeconds, EarlierPausesAndTheOpenOneBothCount) {
    EXPECT_DOUBLE_EQ(CapturedSeconds(20s, 4s, 6s), 10.0);
}

TEST(CapturedSeconds, TheClockNeverReadsBelowZero) {
    // The three inputs are sampled without a lock between them, so a tick can
    // see a pause total from after the instant it measured. A negative duration
    // would print as one.
    EXPECT_DOUBLE_EQ(CapturedSeconds(5s, 6s, kNone), 0.0);
    EXPECT_DOUBLE_EQ(CapturedSeconds(5s, kNone, 6s), 0.0);
    EXPECT_DOUBLE_EQ(CapturedSeconds(kNone, kNone, kNone), 0.0);
}

TEST(CapturedSeconds, SubSecondPrecisionSurvives) {
    // The Record page shows whole seconds, but the finished result carries this
    // number into the media duration comparison.
    EXPECT_DOUBLE_EQ(CapturedSeconds(1500ms, 250ms, kNone), 1.25);
}
