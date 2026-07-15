#pragma once

// Fresh-session stop_requested reset — extracted out of RecorderSession::Record()
// so it can be unit tested against a plain SessionState (no capture hardware, no
// threads required), the same pattern worker_join.h uses for the join wait.
//
// Fixes the "fast start/stop leaves the FinalizingOverlay spinning for a
// long-but-finite time" defect: Stop() can be called before the corresponding
// Record() call has actually begun (RecordingCoordinator sets is_recording_ —
// and therefore accepts a stop click — well before its async prepare phase
// reaches Record()). Record() used to unconditionally reset stop_requested to
// false for the new session, silently discarding that pending stop; the worker
// threads then ran uncontrolled until something else eventually stopped them.

#include "session_internal.h"

namespace recorder_core {

// Resets state's stop_requested for a new Record() call, preserving (instead of
// discarding) a stop that was requested for this upcoming session before Record()
// began. Returns the stop_requested value applied.
inline bool ResetStopRequestedForNewSession(SessionState& state) {
    const bool pre_stop = state.stop_requested_before_next_record.exchange(false);
    state.stop_requested.store(pre_stop);
    if (state.stop_event) {
        if (pre_stop) {
            SetEvent(state.stop_event);
        } else {
            ResetEvent(state.stop_event);
        }
    }
    return pre_stop;
}

} // namespace recorder_core
