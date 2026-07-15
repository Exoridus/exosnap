#include <gtest/gtest.h>

#include "session_stop_reset.h"

using recorder_core::ResetStopRequestedForNewSession;
using recorder_core::SessionState;

// REGRESSION (fast start/stop finalize-hang): a Stop() call that races ahead of
// the next Record() call (StopRecording() fires while the coordinator's async
// prepare phase is still running) must survive Record()'s own state reset
// instead of being silently discarded.
TEST(SessionStopReset, PreservesStopRequestedBeforeRecord) {
    SessionState state;
    // Simulate what RecorderSession::Stop() does when called before Record().
    state.stop_requested.store(true);
    state.stop_requested_before_next_record.store(true);

    const bool initial = ResetStopRequestedForNewSession(state);

    EXPECT_TRUE(initial);
    EXPECT_TRUE(state.stop_requested.load());
    EXPECT_EQ(WaitForSingleObject(state.stop_event, 0), WAIT_OBJECT_0) << "stop_event must stay signaled";
}

// A stale stop_requested left over from the previous (already-finished) session
// must NOT leak into the new one when no stop was requested for this session.
TEST(SessionStopReset, StartsCleanWithoutAPendingStop) {
    SessionState state;
    state.stop_requested.store(true); // stale true from a just-finished session

    const bool initial = ResetStopRequestedForNewSession(state);

    EXPECT_FALSE(initial);
    EXPECT_FALSE(state.stop_requested.load());
    EXPECT_EQ(WaitForSingleObject(state.stop_event, 0), WAIT_TIMEOUT) << "stop_event must be re-armed (unsignaled)";
}

// The pending flag is consumed (one-shot): a second reset without an intervening
// Stop() call must not resurrect the earlier stop.
TEST(SessionStopReset, PendingFlagIsConsumedNotSticky) {
    SessionState state;
    state.stop_requested_before_next_record.store(true);

    ASSERT_TRUE(ResetStopRequestedForNewSession(state));
    const bool second = ResetStopRequestedForNewSession(state);

    EXPECT_FALSE(second);
    EXPECT_FALSE(state.stop_requested.load());
}
