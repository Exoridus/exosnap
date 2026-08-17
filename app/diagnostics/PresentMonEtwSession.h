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
    [[nodiscard]] bool IsOpen() const {
        return open_.load(std::memory_order_acquire);
    }
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
    void SetTargetProcessId(unsigned long pid) {
        target_pid_.store(pid, std::memory_order_relaxed);
        accumulator_.Reset();
        last_present_qpc_ = 0;
        std::lock_guard lk(sample_mutex_);
        latest_ = PresentSample{};
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
    };

    std::atomic<bool> open_{false};
    std::atomic<unsigned long> target_pid_{0};
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
