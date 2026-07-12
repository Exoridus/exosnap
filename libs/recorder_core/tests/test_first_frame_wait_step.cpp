// Pure guard policy for the wait-for-first-frame loop (S3). A game entering
// exclusive fullscreen exactly as recording starts throws ACCESS_LOST before the
// first frame; the loop then enters a bounded start-hold and polls Reopen().
// While that hold is active the 5 s first-frame guard MUST be suspended, or it
// would end the session before the 15 s reopen budget could run. After a
// successful reopen the deadline restarts (fresh 5 s window).
//
// FirstFrameWaitStep is D3D-free and time is passed in, so the guard-suspension
// behaviour is pinned without a live capture. The start-hold reuses the already
// pinned DecideOdReopen budget policy; the start-budget cases are added here too.

#include <recorder_core/dxgi_od_capture_src.h>

#include <chrono>

#include <gtest/gtest.h>

using namespace recorder_core;
using namespace std::chrono_literals;

namespace {

constexpr double kTimeout = 5.0;

TEST(FirstFrameWait, KeepsWaitingBeforeTimeout) {
    EXPECT_EQ(FirstFrameWaitStep(false, 2.0, kTimeout), FirstFrameWaitAction::KeepWaiting);
}

TEST(FirstFrameWait, TimesOutPastGuard) {
    EXPECT_EQ(FirstFrameWaitStep(false, 5.1, kTimeout), FirstFrameWaitAction::TimeoutFail);
}

TEST(FirstFrameWait, ExactlyAtTimeoutStillWaits) {
    // Strictly greater-than fails; at the boundary we give one more poll.
    EXPECT_EQ(FirstFrameWaitStep(false, 5.0, kTimeout), FirstFrameWaitAction::KeepWaiting);
}

TEST(FirstFrameWait, HoldSuspendsTheGuard) {
    // The critical race guard: while the OD start-hold is active, the 5 s timeout
    // is suspended no matter how much time has elapsed — the reopen budget owns
    // the deadline. Without this the 15 s budget would be dead code.
    EXPECT_EQ(FirstFrameWaitStep(true, 12.0, kTimeout), FirstFrameWaitAction::HoldStep);
    EXPECT_EQ(FirstFrameWaitStep(true, 0.0, kTimeout), FirstFrameWaitAction::HoldStep);
}

// ---- start-hold reopen budget (DecideOdReopen with kOdStartHoldBudget) ----

TEST(StartHoldBudget, RetriesWithinFifteenSeconds) {
    const OdReopenDecision d = DecideOdReopen(false, 5s, kOdStartHoldBudget, 250ms);
    EXPECT_EQ(d.action, OdReopenAction::RetryAfter);
    EXPECT_EQ(d.retry_delay, 250ms);
}

TEST(StartHoldBudget, GivesUpAtBudget) {
    const OdReopenDecision d = DecideOdReopen(false, kOdStartHoldBudget, kOdStartHoldBudget, 250ms);
    EXPECT_EQ(d.action, OdReopenAction::GiveUp);
}

TEST(StartHoldBudget, SuccessContinues) {
    const OdReopenDecision d = DecideOdReopen(true, 3s, kOdStartHoldBudget, 250ms);
    EXPECT_EQ(d.action, OdReopenAction::Continue);
}

TEST(StartHoldBudget, RetryDelayClampedToRemainingWindow) {
    const OdReopenDecision d = DecideOdReopen(false, kOdStartHoldBudget - 100ms, kOdStartHoldBudget, 250ms);
    EXPECT_EQ(d.action, OdReopenAction::RetryAfter);
    EXPECT_EQ(d.retry_delay, 100ms);
}

} // namespace
