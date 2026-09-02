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
//
// PendingStopTracker deliberately lives on RecorderSession::Impl (guarded by the
// same state_mutex as `state`/`workers_leaked`), NOT on SessionState: a prior
// version stored this flag on SessionState itself, which broke two ways —
//   1. Stop() set it on EVERY call, not only a call that raced ahead of Record();
//      a Stop() during an ordinary, already-running recording poisoned the NEXT
//      session, which then stopped itself instantly on start.
//   2. When Record() swaps in a fresh SessionState after a leaked worker
//      (workers_leaked), a flag already set on the OLD state was silently
//      dropped instead of being read by the session the swap produced —
//      reproducing the original race in that narrower path.
// Tracking pending-ness on Impl instead of the (possibly-swapped) SessionState
// makes both cases correct by construction: NoteStop() only arms the flag when
// no Record() is actively running for this session, and the flag's lifetime is
// independent of which SessionState object Record() ends up using.

#include "session_internal.h"

#include <utility>

namespace exosnap::engine {

// Tracks a Stop() that must survive into the next Record() call because it
// arrived outside that call's active-recording window (before Record() began,
// or after this Record() call's own capture phase already ended but before it
// returned — e.g. an extra click during finalize). Caller is responsible for
// serializing access (RecorderSession::Impl::state_mutex).
// Whether a stop naming `stop_request` applies to the recording `record_request`.
// The unscoped id matches from either side, so a caller that does not scope its
// calls keeps the plain "stop whatever is running" behaviour.
[[nodiscard]] inline constexpr bool RecordRequestMatches(RecordRequestId record_request,
                                                         RecordRequestId stop_request) noexcept {
    return record_request == kUnscopedRecordRequest || stop_request == kUnscopedRecordRequest ||
           record_request == stop_request;
}

class PendingStopTracker {
  public:
    // Call from Stop() with whether a Record() call is currently actively
    // recording (mid-capture) for this session, and the request the stop names.
    // A stop during active recording is handled entirely by
    // SessionState::stop_requested/stop_event and must NOT be remembered here, or
    // it would poison the next session.
    void NoteStop(bool recording, RecordRequestId request_id) {
        if (!recording) {
            pending_ = true;
            pending_request_ = request_id;
        }
    }

    // One-shot consume: returns whether a stop was pending FOR THIS request, and
    // clears the pending state either way.
    //
    // The id check is what keeps the remembered stop from reaching the wrong
    // recording. Without it every stop landing outside a recording window armed
    // the next Record() call, so a stop belonging to a recording that had already
    // returned killed the following one on start: it began with stop_requested
    // set, captured no frame, and reported a downstream worker's complaint as its
    // cause.
    [[nodiscard]] bool Consume(RecordRequestId request_id) {
        const bool pending = std::exchange(pending_, false);
        const RecordRequestId noted = std::exchange(pending_request_, kUnscopedRecordRequest);
        return pending && RecordRequestMatches(request_id, noted);
    }

    // Drop whatever is pending without applying it.
    void Clear() {
        pending_ = false;
        pending_request_ = kUnscopedRecordRequest;
    }

  private:
    bool pending_ = false;
    RecordRequestId pending_request_ = kUnscopedRecordRequest;
};

// Applies a consumed PendingStopTracker result to a SessionState for a new
// Record() call: pre_stop=true preserves (instead of discarding) a Stop() that
// raced ahead of this Record() call.
inline void ResetStopRequestedForNewSession(SessionState& state, bool pre_stop) {
    state.stop_requested.store(pre_stop);
    if (state.stop_event) {
        if (pre_stop) {
            SetEvent(state.stop_event);
        } else {
            ResetEvent(state.stop_event);
        }
    }
}

} // namespace exosnap::engine
