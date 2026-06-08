#include <gtest/gtest.h>

#include "services/RecordingCountdownController.h"

namespace exosnap {
namespace {

TEST(RecordingCountdownControllerTest, RejectsOffAndUnsupportedDurations) {
    RecordingCountdownController countdown;

    EXPECT_FALSE(countdown.start(0, 0));
    EXPECT_FALSE(countdown.start(4, 0));
    EXPECT_EQ(countdown.state(), CountdownState::Idle);
}

TEST(RecordingCountdownControllerTest, SupportsConfiguredDurations) {
    for (const int seconds : {3, 5, 10}) {
        RecordingCountdownController countdown;

        EXPECT_TRUE(countdown.start(seconds, 100));
        EXPECT_TRUE(countdown.isRunning());
        EXPECT_EQ(countdown.durationSeconds(), seconds);
        EXPECT_EQ(countdown.remainingSeconds(100), seconds);
    }
}

TEST(RecordingCountdownControllerTest, UsesMonotonicElapsedTimeForDisplay) {
    RecordingCountdownController countdown;
    ASSERT_TRUE(countdown.start(3, 1000));

    EXPECT_EQ(countdown.remainingSeconds(1000), 3);
    EXPECT_EQ(countdown.remainingSeconds(1999), 3);
    EXPECT_EQ(countdown.remainingSeconds(2000), 2);
    EXPECT_EQ(countdown.remainingSeconds(2999), 2);
    EXPECT_EQ(countdown.remainingSeconds(3000), 1);
    EXPECT_EQ(countdown.remainingSeconds(3999), 1);
    EXPECT_EQ(countdown.remainingSeconds(4000), 0);
    EXPECT_TRUE(countdown.hasReachedZero(4000));
}

TEST(RecordingCountdownControllerTest, DelayedTicksDoNotDrift) {
    RecordingCountdownController countdown;
    ASSERT_TRUE(countdown.start(5, 0));

    EXPECT_EQ(countdown.remainingSeconds(2350), 3);
    EXPECT_EQ(countdown.remainingSeconds(5010), 0);
    EXPECT_TRUE(countdown.hasReachedZero(5010));
}

TEST(RecordingCountdownControllerTest, DuplicateStartWhileRunningIsRejected) {
    RecordingCountdownController countdown;
    ASSERT_TRUE(countdown.start(3, 0));

    EXPECT_FALSE(countdown.start(5, 500));
    EXPECT_EQ(countdown.durationSeconds(), 3);
    EXPECT_EQ(countdown.remainingSeconds(500), 3);
}

TEST(RecordingCountdownControllerTest, CancelPreventsCompletion) {
    RecordingCountdownController countdown;
    ASSERT_TRUE(countdown.start(3, 0));

    countdown.cancel();

    EXPECT_EQ(countdown.state(), CountdownState::Cancelling);
    EXPECT_FALSE(countdown.hasReachedZero(3000));
    EXPECT_EQ(countdown.remainingSeconds(3000), 0);
}

TEST(RecordingCountdownControllerTest, CompleteAndResetClearState) {
    RecordingCountdownController countdown;
    ASSERT_TRUE(countdown.start(3, 0));

    countdown.complete();
    EXPECT_EQ(countdown.state(), CountdownState::Completed);
    EXPECT_FALSE(countdown.isRunning());

    countdown.reset();
    EXPECT_EQ(countdown.state(), CountdownState::Idle);
    EXPECT_EQ(countdown.durationSeconds(), 0);
    EXPECT_EQ(countdown.remainingSeconds(0), 0);
}

} // namespace
} // namespace exosnap
