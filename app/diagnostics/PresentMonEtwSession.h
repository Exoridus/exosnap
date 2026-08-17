#pragma once

#include "PresentAccumulator.h"
#include "PresentProvider.h"
#include "PresentTraceBackend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace exosnap::diagnostics {

// Owns a real-time present trace and a consumer worker that feeds the classification.
// Latest() returns the most recent mapped present (Unavailable until one is seen).
// The real trace requires elevation; Start() returns false (graceful) when the session
// cannot be opened, and when this build carries no trace backend at all.
//
// This class contains NO conditional compilation. Everything Windows-specific about the
// trace itself lives behind IPresentTraceBackend, so the product and the tests compile
// exactly these lines -- which was not true before: the tests used to compile a no-op
// twin, leaving every contract below verified against an implementation that did
// nothing while the shipping one was verified by nobody.
class PresentMonEtwSession {
  public:
    PresentMonEtwSession();
    // Test seam. The factory is called once per Start(), so each session generation
    // gets its own backend and a detached consumer from a previous generation can
    // never be reached by the next one.
    explicit PresentMonEtwSession(std::function<std::shared_ptr<IPresentTraceBackend>()> backend_factory);
    ~PresentMonEtwSession();
    PresentMonEtwSession(const PresentMonEtwSession&) = delete;
    PresentMonEtwSession& operator=(const PresentMonEtwSession&) = delete;

    [[nodiscard]] bool Start();
    void Stop();
    // True only while a consumer is actually consuming. Not "Start() succeeded":
    // `open_` is written by Start() and Stop() and by nobody else, so a Consume() that
    // returned for any other reason -- the session torn down by another process, a
    // driver reset, an ETW buffer error -- would leave it true forever while Latest()
    // went on serving the last sample it ever saw AS THE CURRENT ONE. A dead trace
    // reporting a live present mode is worse than reporting none.
    //
    // It is therefore also false for a moment AFTER Start() returns true, until the
    // consumer thread has actually entered Consume(). That gap is truthful: an open
    // trace nobody is reading yet has nothing to report, and Latest() has no sample to
    // return in it either.
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

    // Test-only observers. They report the two facts a caller cannot otherwise see
    // without a real trace, and they exist so the contracts stay checkable rather than
    // to give production code a second way to ask.
    [[nodiscard]] unsigned long TargetProcessIdForTest() const {
        return target_pid_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] PresentAccumulator AccumulatorForTest() const {
        return accumulator_;
    }

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
        // Set by the consumer thread when it enters Consume() and cleared when Consume()
        // returns, whichever reason it returned for. Starts FALSE on purpose: claiming a
        // consumer before one is running would make IsOpen() a statement about Start()'s
        // return value rather than about the trace.
        std::atomic<bool> consuming{false};
        // Set by Stop() immediately before Close(). Lets the consumer thread tell
        // "I was asked to end" from "the trace ended under me", which are the same
        // Consume() return and very different facts about the machine.
        std::atomic<bool> stop_requested{false};
    };

    // True while the process this session is attributed to is still running. pid 0 --
    // "whatever dominates the screen" -- has nothing to outlive and is always alive.
    [[nodiscard]] bool TargetAlive() const;

    std::function<std::shared_ptr<IPresentTraceBackend>()> backend_factory_;
    std::atomic<bool> open_{false};
    std::atomic<unsigned long> target_pid_{0};
    // A SYNCHRONIZE handle on the attributed process, nullptr for pid 0 or for a
    // process this one may not open. HELD rather than re-opened per sample, because
    // Windows recycles process ids: a pid re-checked after the target exited can name
    // a different program, and the attribution would silently follow it. Guarded by
    // sample_mutex_; typed void* to keep the header free of windows.h.
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
    // The trace for THIS session generation. Re-created by every Start() and released by
    // Stop(), so a consumer detached after the shutdown ceiling still owns everything it
    // touches and cannot collide with the next generation. Guarded by sample_mutex_.
    std::shared_ptr<IPresentTraceBackend> backend_;
};

} // namespace exosnap::diagnostics
