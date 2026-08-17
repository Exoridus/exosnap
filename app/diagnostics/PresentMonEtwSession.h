#pragma once

#include "PresentAccumulator.h"
#include "PresentProvider.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace exosnap::diagnostics {

// Owns a real-time ETW present-trace session and a consumer worker that feeds the
// vendored PresentMon PresentData decoder. Latest() returns the most recent mapped
// present (Unavailable until one is seen). Requires elevation; Start() returns false
// (graceful) when the session cannot be opened.
class PresentMonEtwSession {
  public:
    PresentMonEtwSession();
    ~PresentMonEtwSession();
    PresentMonEtwSession(const PresentMonEtwSession&) = delete;
    PresentMonEtwSession& operator=(const PresentMonEtwSession&) = delete;

    [[nodiscard]] bool Start();
    void Stop();
    // True only while a consumer is actually consuming. `open_` alone is NOT that
    // answer: it is written by Start() and Stop() and by nobody else, so a
    // ProcessTrace that returned for any other reason -- the session torn down by
    // another process, a driver reset, an ETW buffer error -- left this true forever
    // while Latest() went on serving the last sample it ever saw AS THE CURRENT ONE.
    // A dead trace reporting a live present mode is worse than reporting none.
    [[nodiscard]] bool IsOpen() const;
    [[nodiscard]] PresentSample Latest() const;
    // Scope reporting to a capture target's process (0 = dominant non-composed presenter).
    // Every call marks an attribution boundary (recording start/stop, or an idle
    // capture-target change), so it also resets the per-recording accumulators: the
    // present/discard/flip statistics then describe ONLY the source targeted from this
    // point on. The reset is UNCONDITIONAL — Monitor/Region recordings target pid 0
    // exactly like the idle desktop, so a pid-equality guard would let idle totals leak
    // into them (diluting a real discard problem, or latching the absolute flip counter).
    // Called on the same (UI) thread as Latest(), so the reader-side accumulator access
    // is race-free; latest_ is cleared under sample_mutex_ so no stale cumulative sample
    // is returned before the next drain repopulates it.
    //
    // Also takes a SYNCHRONIZE handle on the process, so that a target which EXITS
    // between two boundaries is noticed. Without it the pid filter simply stops
    // matching, no further present ever replaces the last one, and a game the user
    // closed keeps describing the machine's current present mode.
    void SetTargetProcessId(unsigned long pid);

  private:
    // Handshake between the consumer thread and Stop(), owned by a shared_ptr that
    // BOTH hold. It cannot live in this object: when the bounded wait below times
    // out the thread is detached, and a detached thread that then signalled a member
    // of a destroyed session would be a use-after-free -- the exact trade a timeout
    // is supposed to avoid.
    struct FinishSignal {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
        // Cleared by the consumer thread the instant ProcessTrace returns, whichever
        // reason it returned for. Lives here rather than in the session for the same
        // reason `finished` does: after a detach the thread may outlive the session,
        // and it still has to be able to say it stopped. Atomic because IsOpen() reads
        // it without taking `mutex`.
        std::atomic<bool> consuming{true};
        // Set by Stop() immediately before CloseTrace. Lets the consumer thread tell
        // "I was asked to end" from "the trace ended under me", which are the same
        // ProcessTrace return and very different facts about the machine.
        std::atomic<bool> stop_requested{false};
    };

    // True while the process this session is attributed to is still running. pid 0 --
    // "whatever dominates the screen" -- has nothing to outlive and is always alive.
    [[nodiscard]] bool TargetAlive() const;

    std::atomic<bool> open_{false};
    std::atomic<unsigned long> target_pid_{0};
    // A SYNCHRONIZE handle on the attributed process, nullptr for pid 0 or for a
    // process this one may not open. HELD rather than re-opened per sample, because
    // Windows recycles process ids: a pid re-checked after the target exited can name
    // a different program, and the attribution would silently follow it. Guarded by
    // sample_mutex_; typed void* to keep the header free of windows.h, like impl_.
    void* target_handle_ = nullptr;
    // One log line per dead target, not one per sample.
    mutable bool target_death_logged_ = false;
    std::shared_ptr<FinishSignal> finish_; // guarded by sample_mutex_ for publication
    std::thread worker_;
    mutable std::mutex sample_mutex_;
    mutable PresentSample latest_;          // guarded by sample_mutex_
    mutable uint64_t last_present_qpc_ = 0; // reader-side drain state (Latest())
    mutable int64_t qpc_freq_ = 0;
    // ADR 0033 extra-checks: per-recording present aggregates, accumulated on the reader
    // side across the drain (same single-threaded Latest() access as last_present_qpc_) and
    // Reset() at every attribution boundary via SetTargetProcessId so the statistics measure
    // only the current recording, never the whole session.
    mutable PresentAccumulator accumulator_;
    // Opaque SessionImpl (PMTraceConsumer + TraceSession). shared_ptr<void> keeps the
    // header PresentMon-free; all access is guarded by sample_mutex_. Snapshotting the
    // pointer under the lock keeps SessionImpl alive across a concurrent Stop().
    std::shared_ptr<void> impl_;
};

} // namespace exosnap::diagnostics
