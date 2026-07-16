#include <gtest/gtest.h>

#include "session_stop_reset.h"

using recorder_core::PendingStopTracker;
using recorder_core::ResetStopRequestedForNewSession;
using recorder_core::SessionState;

// ---------------------------------------------------------------------------
// ResetStopRequestedForNewSession — pure application of a consumed pre_stop
// onto a fresh SessionState.
// ---------------------------------------------------------------------------

TEST(ResetStopRequestedForNewSession, AppliesPendingStop) {
    SessionState state;

    ResetStopRequestedForNewSession(state, /*pre_stop=*/true);

    EXPECT_TRUE(state.stop_requested.load());
    EXPECT_EQ(WaitForSingleObject(state.stop_event, 0), WAIT_OBJECT_0) << "stop_event must stay signaled";
}

TEST(ResetStopRequestedForNewSession, StartsCleanWithoutAPendingStop) {
    SessionState state;
    state.stop_requested.store(true); // stale true from a just-finished session
    SetEvent(state.stop_event);

    ResetStopRequestedForNewSession(state, /*pre_stop=*/false);

    EXPECT_FALSE(state.stop_requested.load());
    EXPECT_EQ(WaitForSingleObject(state.stop_event, 0), WAIT_TIMEOUT) << "stop_event must be re-armed (unsignaled)";
}

// ---------------------------------------------------------------------------
// PendingStopTracker — the Impl-level state machine that decides whether a
// Stop() call must survive into the next Record() call.
// ---------------------------------------------------------------------------

// REGRESSION (fast start/stop finalize-hang): a Stop() call that races ahead of
// the next Record() call (StopRecording() fires while the coordinator's async
// prepare phase is still running, i.e. no Record() is actively recording yet)
// must survive Record()'s own state reset instead of being silently discarded.
TEST(PendingStopTracker, StopBeforeRecordingSurvivesIntoNextRecord) {
    PendingStopTracker tracker;

    tracker.NoteStop(/*recording=*/false);

    EXPECT_TRUE(tracker.Consume());
}

// REGRESSION: a Stop() call during an ALREADY-ACTIVE recording (the ordinary
// case — stop_requested/stop_event on the current SessionState handle it) must
// NOT be remembered, or it would poison the very next recording by making it
// stop itself instantly on start.
TEST(PendingStopTracker, StopDuringActiveRecordingDoesNotLeakForward) {
    PendingStopTracker tracker;

    tracker.NoteStop(/*recording=*/true);

    EXPECT_FALSE(tracker.Consume());
}

// The pending flag is consumed (one-shot): a second consume without an
// intervening NoteStop() must not resurrect the earlier stop.
TEST(PendingStopTracker, ConsumeIsOneShot) {
    PendingStopTracker tracker;
    tracker.NoteStop(/*recording=*/false);

    ASSERT_TRUE(tracker.Consume());

    EXPECT_FALSE(tracker.Consume());
}

// A no-op default state must not report a pending stop.
TEST(PendingStopTracker, StartsClean) {
    PendingStopTracker tracker;

    EXPECT_FALSE(tracker.Consume());
}
