#pragma once

// Two-phase cooperative shutdown wait for the recorder's worker threads.
//
// This logic is factored out of RecorderSession::Record() so it can be unit
// tested with deterministic std::threads (no capture hardware required). It
// encapsulates the fix for a long-standing defect where the join budget started
// ticking at recording START rather than at STOP, imposing a hard cap on the
// recording duration: any recording longer than the budget (120 s) failed with
// ERROR_TIMEOUT and left an unfinalized file.

#include <atomic>
#include <vector>

#include <windows.h>

namespace recorder_core {

struct WorkerJoinResult {
    // True iff every handle was signaled (worker exited) by the time the wait
    // returned.
    bool all_signaled = false;
    // Per-handle signaled status, in the same order as the input handles.
    std::vector<bool> signaled;
    // A WaitForMultipleObjects call returned WAIT_FAILED.
    bool wait_failed = false;
    // GetLastError() captured when wait_failed is true (0 otherwise).
    DWORD last_error = 0;
};

// Wait for the worker threads to finish, in two phases:
//
//   Phase 1 (unbounded): block until `stop_requested` becomes true OR all worker
//     handles have already been signaled on their own. The worker threads only
//     begin to drain and exit once stop_requested is set (by Stop() or a fatal
//     worker failure), so this wait MUST be unbounded — applying the join budget
//     here would cap the recording duration. The bWaitAll probe doubles as a
//     guard for the unlikely case where a worker exits without setting
//     stop_requested: if all handles are already signaled we fall through.
//
//   Phase 2 (bounded): once a stop has been requested, give all workers a single
//     shared `join_budget_ms` budget to drain their queues and finalize output.
//
// `handles` must contain no null entries (callers validate this beforehand).
// The function does not join the std::thread wrappers — the caller does that for
// any handle reported as signaled so a still-running thread is never joined.
inline WorkerJoinResult WaitForWorkersThenJoin(const std::vector<HANDLE>& handles,
                                               const std::atomic<bool>& stop_requested, DWORD join_budget_ms,
                                               DWORD stop_poll_interval_ms) {
    WorkerJoinResult result;
    result.signaled.assign(handles.size(), false);
    if (handles.empty()) {
        result.all_signaled = true;
        return result;
    }

    const DWORD count = static_cast<DWORD>(handles.size());

    // Phase 1 — run until shutdown is requested or all workers exit on their own.
    while (!stop_requested.load()) {
        const DWORD w = WaitForMultipleObjects(count, handles.data(), TRUE, stop_poll_interval_ms);
        if (w == WAIT_TIMEOUT) {
            continue; // still recording — keep waiting, no budget consumed
        }
        if (w == WAIT_FAILED) {
            result.wait_failed = true;
            result.last_error = GetLastError();
            return result;
        }
        break; // all handles already signaled
    }

    // Phase 2 — bounded join now that a stop has been requested.
    const DWORD joinWait = WaitForMultipleObjects(count, handles.data(), TRUE, join_budget_ms);
    if (joinWait == WAIT_FAILED) {
        result.wait_failed = true;
        result.last_error = GetLastError();
        return result;
    }

    // Probe each handle (non-blocking) for its final signaled status.
    bool all = true;
    for (DWORD i = 0; i < count; ++i) {
        const bool sig = (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0);
        result.signaled[i] = sig;
        if (!sig) {
            all = false;
        }
    }
    result.all_signaled = all;
    return result;
}

} // namespace recorder_core
