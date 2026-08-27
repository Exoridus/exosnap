#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "session_stop_reset.h"

using exosnap::engine::PendingStopTracker;
using exosnap::engine::ResetStopRequestedForNewSession;
using exosnap::engine::SessionState;

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

// ---------------------------------------------------------------------------
// capture_end_ns -- when the CAPTURE ended, as opposed to when Record() returns.
//
// The reported duration used to be measured to the end of Record(), which is
// after the producer drain and the whole container finalize. Both are work done
// after the last captured frame, both are O(duration) and disk-bound, and the
// elapsed time therefore jumped forward at Stop by however long finalizing took.
// ---------------------------------------------------------------------------

TEST(CaptureEndInstant, AFreshSessionHasNotRecordedOne) {
    SessionState state;
    EXPECT_EQ(state.capture_end_ns.load(), 0) << "a session that has not stopped must not claim an end";
}

TEST(CaptureEndInstant, TheFinalizeThatFollowsDoesNotMoveIt) {
    SessionState state;
    state.NoteCaptureEnded();
    const int64_t at_stop = state.capture_end_ns.load();
    ASSERT_NE(at_stop, 0);

    // Stands in for the drain and the finalize: everything Record() still does
    // after the capture is over.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    state.NoteCaptureEnded();

    EXPECT_EQ(state.capture_end_ns.load(), at_stop) << "the instant moved while the container was still being written";
}

// A failure raised while a user stop is already draining must not re-date the
// recording: RecordFailure raises the same stop token and would otherwise claim
// the later instant.
TEST(CaptureEndInstant, AFailureDuringTheDrainKeepsTheFirstInstant) {
    SessionState state;
    state.NoteCaptureEnded();
    const int64_t at_stop = state.capture_end_ns.load();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state.RecordFailure(1, exosnap::engine::ErrorPhase::Shutdown, "late failure");

    EXPECT_EQ(state.capture_end_ns.load(), at_stop);
}
