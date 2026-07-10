// Pure policy for the DXGI Output Duplication live re-duplication retry loop.
// After ClassifyOdAcquireFailure returns Recover (DXGI_ERROR_ACCESS_LOST: the
// duplication handle is stale but the D3D device is alive), the drain rebuilds
// the duplication and continues the SAME encode session. The output can be
// briefly absent while the topology renegotiation that caused the loss settles
// (e.g. an attached display waking, EDID/HPD re-negotiation blacks every screen
// for a moment), so a single Reopen() attempt may fail. This function decides,
// from the last attempt's result and the time elapsed since the loss began,
// whether to continue (recovered), wait and retry (still within budget), or give
// up (budget exhausted -> clean stop, the historic source-loss behaviour).
//
// The decision is D3D-free and pinned here; time is passed in so no wall clock
// is read in the test:
//   reopened                     -> Continue   (success always wins)
//   still failing, budget unset  -> RetryAfter (unbounded: keep trying forever)
//   still failing, in  budget    -> RetryAfter (wait, then try again)
//   still failing, past budget   -> GiveUp     (end the recording cleanly)
// The drain uses an unbounded budget (std::nullopt) so a recoverable loss never
// ends the recording on a timer — only an explicit user stop / unrecoverable
// failure does. The bounded branch remains supported and pinned below.

#include "dxgi_od_capture_src.h"

#include <chrono>
#include <optional>

#include <gtest/gtest.h>

using namespace recorder_core;
using namespace std::chrono_literals;

namespace {

// A representative recovery budget / poll cadence for these pins. The concrete
// values the drain uses are its own; the policy is budget-agnostic.
constexpr auto kBudget = 8000ms;
constexpr auto kPoll = 250ms;

TEST(OdReopenPolicy, ImmediateSuccessContinues) {
    // The duplication came back on the first attempt: resume the same session.
    const OdReopenDecision d = DecideOdReopen(true, 0ms, kBudget, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::Continue);
}

TEST(OdReopenPolicy, RepeatedFailureWithinBudgetRetries) {
    // Output still absent but well within the budget: wait the poll delay, retry.
    const OdReopenDecision d = DecideOdReopen(false, 500ms, kBudget, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::RetryAfter);
    EXPECT_EQ(d.retry_delay, kPoll);
}

TEST(OdReopenPolicy, FailureAtBudgetGivesUp) {
    // Elapsed has reached the budget without recovery: clean stop (source-loss).
    const OdReopenDecision d = DecideOdReopen(false, kBudget, kBudget, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::GiveUp);
}

TEST(OdReopenPolicy, FailurePastBudgetGivesUp) {
    const OdReopenDecision d = DecideOdReopen(false, kBudget + 1s, kBudget, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::GiveUp);
}

TEST(OdReopenPolicy, SuccessWinsEvenPastBudget) {
    // A late Reopen() that finally succeeds must resume the session rather than be
    // discarded as a timeout — the footage is worth more than a strict deadline.
    const OdReopenDecision d = DecideOdReopen(true, kBudget + 5s, kBudget, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::Continue);
}

TEST(OdReopenPolicy, RetryDelayNeverOvershootsBudget) {
    // Near the end of the budget the remaining window is shorter than one poll
    // interval: the retry delay is clamped so the loop cannot sleep past the
    // budget and stall the clean-stop decision.
    const OdReopenDecision d = DecideOdReopen(false, kBudget - 100ms, kBudget, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::RetryAfter);
    EXPECT_EQ(d.retry_delay, 100ms);
}

TEST(OdReopenPolicy, UnboundedNeverGivesUp) {
    // With no budget (std::nullopt) the retry is unbounded: even far past any
    // reasonable deadline the loop keeps retrying at the full poll cadence rather
    // than giving up. This is the drain's configuration — a recoverable loss ends
    // the recording only on an explicit user stop / unrecoverable failure.
    const OdReopenDecision d = DecideOdReopen(false, 10min, std::nullopt, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::RetryAfter);
    EXPECT_EQ(d.retry_delay, kPoll);
}

TEST(OdReopenPolicy, UnboundedSuccessContinues) {
    // Success still wins immediately even with no budget in play.
    const OdReopenDecision d = DecideOdReopen(true, 10min, std::nullopt, kPoll);
    EXPECT_EQ(d.action, OdReopenAction::Continue);
}

} // namespace
