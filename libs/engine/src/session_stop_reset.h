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
#include "session_request_gate.h"

namespace exosnap::engine {

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
