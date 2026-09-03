#pragma once

// Outbound-callback gate for one Record() call -- extracted so it can be unit
// tested without capture hardware, the same pattern session_stop_reset.h uses.
//
// A worker that misses its join is abandoned, not killed: it keeps running on
// its own SessionState (see RecorderSession::Impl) and eventually returns from
// whatever it was blocked in. The callbacks that state carries -- segment
// finalized, preview handle, snapshot ready -- point back into the caller
// (the app's recording coordinator, bound to THIS session's bookkeeping). Left
// unguarded, a late return fires them into whichever session is running by
// then: a segment of recording N appended to recording N+1's segment list, its
// split flag cleared, a manifest entry written under N+1's id; or, after the
// coordinator is gone, a call through a dangling pointer.
//
// Every callback a worker thread can fire is therefore installed through a
// gate that Record() closes before it returns. A late caller finds the gate
// closed and does nothing. The gate is per Record() call (a fresh object each
// time) so a still-running abandoned worker holds the OLD gate: closing it can
// never race the next session's open one, and no lock is needed to open.
//
// Close() releases the wrapped callbacks so the gate does not keep the caller's
// objects reachable, but only if no invocation is in flight within a short
// bound. An invocation that is still running (a callback blocked on the same
// stalled disk that stalled the worker) keeps its callbacks alive and is left
// to finish on its own -- destroying a std::function under a running call is
// undefined behaviour, while a never-again-invoked callback holding a stale
// pointer is inert. This is also why Close() must never wait unboundedly: it
// runs on the Record() thread, and a stop that cannot return is the defect the
// abandon policy exists to prevent.

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <utility>

namespace exosnap::engine {

class SessionCallbackGate {
  public:
    // Runs `fn` unless the gate is closed. Concurrent invocations do not block
    // each other; only Close() excludes them.
    template <typename Fn> void Invoke(Fn&& fn) {
        if (!open_.load(std::memory_order_acquire))
            return;
        std::shared_lock lk(mutex_);
        if (!open_.load(std::memory_order_relaxed))
            return;
        std::forward<Fn>(fn)();
    }

    // Closes the gate for good. Returns true when no invocation was in flight
    // within `drain_bound` (so the caller may drop what the callbacks reference),
    // false when one still is.
    bool Close(std::chrono::milliseconds drain_bound = std::chrono::milliseconds(2000)) {
        open_.store(false, std::memory_order_release);
        if (!mutex_.try_lock_for(drain_bound))
            return false;
        mutex_.unlock();
        return true;
    }

    [[nodiscard]] bool IsOpen() const noexcept {
        return open_.load(std::memory_order_acquire);
    }

  private:
    std::shared_timed_mutex mutex_;
    std::atomic<bool> open_{true};
};

} // namespace exosnap::engine
