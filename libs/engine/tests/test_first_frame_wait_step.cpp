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

#include <exosnap/engine/dxgi_od_capture_src.h>

#include <chrono>

#include <gtest/gtest.h>

using namespace exosnap::engine;
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

// ---- the overall bound the two windows above do not have between them ----
//
// The first-frame deadline restarts after every successful reopen, and the hold
// budget restarts on every entry into a hold. Each window is bounded; their
// alternation is not. A display that reopens and immediately loses access again
// keeps both windows young for as long as it cares to. The overall clock is
// measured from the first attempt and nothing resets it.

namespace {

constexpr double kOverall = std::chrono::duration<double>(kFirstFrameOverallBudget).count();

TEST(FirstFrameOverallBudget, OrdinaryWaitIsUnaffected) {
    EXPECT_EQ(FirstFrameWaitStepBounded(false, 2.0, kTimeout, 2.0, kOverall), FirstFrameWaitAction::KeepWaiting);
    EXPECT_EQ(FirstFrameWaitStepBounded(false, 5.1, kTimeout, 5.1, kOverall), FirstFrameWaitAction::TimeoutFail);
    EXPECT_EQ(FirstFrameWaitStepBounded(true, 12.0, kTimeout, 12.0, kOverall), FirstFrameWaitAction::HoldStep);
}

TEST(FirstFrameOverallBudget, ASingleRecoveredHoldFits) {
    // One fullscreen switch: 14 s of hold, reopen succeeds, the frame arrives 4 s
    // later. Both windows were honoured and the overall bound was not reached.
    EXPECT_EQ(FirstFrameWaitStepBounded(false, 4.0, kTimeout, 18.0, kOverall), FirstFrameWaitAction::KeepWaiting);
}

TEST(FirstFrameOverallBudget, TheOverallBoundIsNotSuspendedByAHold) {
    // This is the defect. While holding, the 5 s guard is suspended by design --
    // and with it, before this, every bound there was. The overall bound has to
    // end a hold that has gone on long enough, whatever the hold's own budget says.
    EXPECT_EQ(FirstFrameWaitStepBounded(true, 0.0, kTimeout, kOverall + 0.1, kOverall),
              FirstFrameWaitAction::TimeoutFail);
}

TEST(FirstFrameOverallBudget, AFreshDeadlineDoesNotEscapeTheOverallBound) {
    // Just reopened: the first-frame window is brand new. The overall clock is not.
    EXPECT_EQ(FirstFrameWaitStepBounded(false, 0.1, kTimeout, kOverall + 0.1, kOverall),
              FirstFrameWaitAction::TimeoutFail);
}

TEST(FirstFrameOverallBudget, AlternatingReopenAndLossTerminates) {
    // The scenario itself, driven by a fake clock. Every cycle: a hold that
    // almost exhausts its budget, a reopen that succeeds, a fresh first-frame
    // window, and an ACCESS_LOST before a frame arrives. Each window is honoured
    // on its own terms; only the overall bound can end the sequence.
    double overall = 0.0;
    int cycles = 0;
    for (;;) {
        // Hold phase: the per-hold budget is never exceeded, so DecideOdReopen
        // would keep retrying and then Continue -- unless the overall bound fires.
        const double holdSeconds = std::chrono::duration<double>(kOdStartHoldBudget).count() - 0.5;
        overall += holdSeconds;
        if (FirstFrameWaitStepBounded(true, 0.0, kTimeout, overall, kOverall) == FirstFrameWaitAction::TimeoutFail)
            break;
        // Reopen succeeded: fresh first-frame window, then the display is lost
        // again 1 s in, before the 5 s guard could have fired.
        overall += 1.0;
        if (FirstFrameWaitStepBounded(false, 1.0, kTimeout, overall, kOverall) == FirstFrameWaitAction::TimeoutFail)
            break;
        ++cycles;
        ASSERT_LT(cycles, 1000) << "the start never terminated against a display that alternates reopen and loss";
    }
    EXPECT_GE(cycles, 1) << "one recovered hold must be allowed before the overall bound ends the start";
    EXPECT_LE(cycles, 2) << "the overall bound must end the start within a few cycles";
    EXPECT_LE(overall, kOverall + std::chrono::duration<double>(kOdStartHoldBudget).count())
        << "the start overran the overall budget by more than one hold window";
}

TEST(FirstFrameOverallBudget, TheOverallBudgetExceedsTwoHoldsAndAGuard) {
    // The constant's stated reasoning, held by a test: a start that recovers once
    // needs a hold, a guard and some slack; a start that needs two full holds
    // is a display that is not settling.
    const double hold = std::chrono::duration<double>(kOdStartHoldBudget).count();
    EXPECT_GT(kOverall, hold + kTimeout);
    EXPECT_GT(kOverall, 2.0 * hold);
    EXPECT_LT(kOverall, 3.0 * hold);
}

} // namespace
