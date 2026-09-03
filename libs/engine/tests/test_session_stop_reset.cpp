#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "session_stop_reset.h"

using exosnap::engine::kUnscopedRecordRequest;
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

    tracker.NoteStop(/*recording=*/false, kUnscopedRecordRequest);

    EXPECT_TRUE(tracker.Consume(kUnscopedRecordRequest));
}

// REGRESSION: a Stop() call during an ALREADY-ACTIVE recording (the ordinary
// case — stop_requested/stop_event on the current SessionState handle it) must
// NOT be remembered, or it would poison the very next recording by making it
// stop itself instantly on start.
TEST(PendingStopTracker, StopDuringActiveRecordingDoesNotLeakForward) {
    PendingStopTracker tracker;

    tracker.NoteStop(/*recording=*/true, kUnscopedRecordRequest);

    EXPECT_FALSE(tracker.Consume(kUnscopedRecordRequest));
}

// The pending flag is consumed (one-shot): a second consume without an
// intervening NoteStop() must not resurrect the earlier stop.
TEST(PendingStopTracker, ConsumeIsOneShot) {
    PendingStopTracker tracker;
    tracker.NoteStop(/*recording=*/false, kUnscopedRecordRequest);

    ASSERT_TRUE(tracker.Consume(kUnscopedRecordRequest));

    EXPECT_FALSE(tracker.Consume(kUnscopedRecordRequest));
}

// A no-op default state must not report a pending stop.
TEST(PendingStopTracker, StartsClean) {
    PendingStopTracker tracker;

    EXPECT_FALSE(tracker.Consume(kUnscopedRecordRequest));
}

// REGRESSION (recording dies on start with zero frames): a stop belonging to a
// recording that has already ended lands outside any recording window and is
// remembered like a stop that raced ahead of the NEXT Record() call. Only the
// request id tells the two apart, and the next recording must survive it.
TEST(PendingStopTracker, AStopFromAnEarlierRecordingDoesNotArmTheNextOne) {
    PendingStopTracker tracker;

    tracker.NoteStop(/*recording=*/false, /*request_id=*/7);

    EXPECT_FALSE(tracker.Consume(/*request_id=*/8));
}

// The stop a user presses while THIS recording is still preparing carries this
// recording's id, and must still be applied to it.
TEST(PendingStopTracker, AStopForThisRecordingStillArmsIt) {
    PendingStopTracker tracker;

    tracker.NoteStop(/*recording=*/false, /*request_id=*/8);

    EXPECT_TRUE(tracker.Consume(/*request_id=*/8));
}

// A mismatching consume must also clear the pending stop: leaving it armed would
// only defer the damage to whichever recording happened to reuse the id.
TEST(PendingStopTracker, AMismatchedConsumeStillClearsThePendingStop) {
    PendingStopTracker tracker;
    tracker.NoteStop(/*recording=*/false, /*request_id=*/7);

    ASSERT_FALSE(tracker.Consume(/*request_id=*/8));

    EXPECT_FALSE(tracker.Consume(/*request_id=*/7));
}

// An unscoped caller (tools, tests, anything strictly sequential) keeps the
// plain "stop whatever comes next" behaviour from either side.
TEST(PendingStopTracker, TheUnscopedRequestMatchesFromEitherSide) {
    PendingStopTracker unscoped_stop;
    unscoped_stop.NoteStop(/*recording=*/false, kUnscopedRecordRequest);
    EXPECT_TRUE(unscoped_stop.Consume(/*request_id=*/8));

    PendingStopTracker unscoped_record;
    unscoped_record.NoteStop(/*recording=*/false, /*request_id=*/8);
    EXPECT_TRUE(unscoped_record.Consume(kUnscopedRecordRequest));
}

// Clear() is what Record() uses to drop a stop aimed at the session it just
// finished; nothing may survive it.
TEST(PendingStopTracker, ClearDropsAPendingStop) {
    PendingStopTracker tracker;
    tracker.NoteStop(/*recording=*/false, /*request_id=*/8);

    tracker.Clear();

    EXPECT_FALSE(tracker.Consume(/*request_id=*/8));
}

// ---------------------------------------------------------------------------
// NextRecordRequestId -- ids must be unique and never collide with the unscoped
// sentinel, which matches everything.
// ---------------------------------------------------------------------------

TEST(RecordRequestId, MintedIdsAreDistinctAndNeverUnscoped) {
    const auto first = exosnap::engine::NextRecordRequestId();
    const auto second = exosnap::engine::NextRecordRequestId();

    EXPECT_NE(first, second);
    EXPECT_NE(first, kUnscopedRecordRequest);
    EXPECT_NE(second, kUnscopedRecordRequest);
}

// ---------------------------------------------------------------------------
// SessionState::ResetForNewRecording -- a state object reused by the next
// Record() call must be indistinguishable from a fresh one. Record() only swaps
// in a new SessionState after a worker had to be abandoned, so reuse is the
// normal path and every field that survives it is a value the next recording
// silently inherits.
// ---------------------------------------------------------------------------

// REGRESSION: NoteCaptureEnded records the instant exactly once (a
// compare-exchange against 0). A value left from the previous recording made the
// next stop a no-op, so the new session reported the OLD session's capture end
// and its duration -- measured from this session's start to an earlier instant
// -- came out negative and was clamped to zero.
TEST(SessionStateReuse, ResetsTheCaptureEndInstant) {
    SessionState state;
    state.NoteCaptureEnded();
    ASSERT_NE(state.capture_end_ns.load(), 0);

    state.ResetForNewRecording();
    ASSERT_EQ(state.capture_end_ns.load(), 0) << "a reused session must not carry the previous capture end";

    state.NoteCaptureEnded();
    EXPECT_NE(state.capture_end_ns.load(), 0) << "the new session's stop must be able to record its own instant";
}

// REGRESSION: live mute is per-session -- a new recording starts from the source
// rows. A mute still standing when the previous recording stopped started the
// next one's microphone silently muted.
TEST(SessionStateReuse, ResetsLiveMute) {
    SessionState state;
    state.audio_mute_mask.store(0b101);

    state.ResetForNewRecording();

    EXPECT_EQ(state.audio_mute_mask.load(), 0u);
}

// REGRESSION: paused time is subtracted from elapsed time. The previous
// session's pause was subtracted from the next session's duration, reporting a
// recording shorter than the file.
TEST(SessionStateReuse, ResetsAccumulatedPauseTime) {
    SessionState state;
    state.paused_ns.store(5'000'000'000LL);

    state.ResetForNewRecording();

    EXPECT_EQ(state.paused_ns.load(), 0);
}

// The finalize-stall detector reads mux_bytes_written for byte progress; a total
// left from the previous file reads as progress this one has not made.
TEST(SessionStateReuse, ResetsWrittenBytes) {
    SessionState state;
    state.mux_bytes_written.store(4096);

    state.ResetForNewRecording();

    EXPECT_EQ(state.mux_bytes_written.load(), 0u);
}

TEST(SessionStateReuse, ResetsTheRecordedFailure) {
    SessionState state;
    state.RecordFailure(1, exosnap::engine::ErrorPhase::Mux, "previous session");

    state.ResetForNewRecording();

    EXPECT_FALSE(state.HasFailure());
}

TEST(SessionStateReuse, ResetsPauseAndSplitState) {
    SessionState state;
    state.pause_requested.store(true);
    state.split_request_seq.store(3);
    state.size_split_armed.store(true);

    state.ResetForNewRecording();

    EXPECT_FALSE(state.pause_requested.load());
    EXPECT_EQ(state.split_request_seq.load(), 0u);
    EXPECT_FALSE(state.size_split_armed.load());
}

// ---------------------------------------------------------------------------
// RequestCleanStop -- the single way to stop a session. A bare
// stop_requested.store(true) reaches the flag and leaves every other invariant
// of a stop undone.
// ---------------------------------------------------------------------------

TEST(RequestCleanStop, RaisesTheFlagTheEventAndTheCaptureEnd) {
    SessionState state;

    state.RequestCleanStop();

    EXPECT_TRUE(state.stop_requested.load());
    EXPECT_NE(state.capture_end_ns.load(), 0) << "the duration is measured to this instant";
    EXPECT_EQ(WaitForSingleObject(state.stop_event, 0), WAIT_OBJECT_0)
        << "a producer blocked in WaitForMultipleObjects must wake now, not on its timeout";
}

// A clean stop is not a failure: nothing may be reported as the session's cause.
TEST(RequestCleanStop, RecordsNoFailure) {
    SessionState state;

    state.RequestCleanStop();

    EXPECT_FALSE(state.HasFailure());
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

// The caller-stop mark is what tells "the user stopped a recording that had no
// frame yet" from "the capture delivered nothing" (session_outcome.h). It is
// per recording: one left standing would make the next zero-frame session
// look like a user abort.
TEST(ResetForNewRecording, ClearsTheCallerStopMark) {
    SessionState state;
    state.caller_stop_requested.store(true);

    state.ResetForNewRecording();

    EXPECT_FALSE(state.caller_stop_requested.load());
}
