#pragma once

#include "PresentModeMapping.h"
#include "PresentProvider.h"

#include <atomic>
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
    void SetTargetProcessId(unsigned long pid) {
        target_pid_.store(pid, std::memory_order_relaxed);
    }

  private:
    void ConsumeLoop();                              // runs ProcessTrace (blocking) on worker_
    void OnPresentCompleted(const RawPresentEvent&); // called from the decoder

    std::atomic<bool> open_{false};
    std::atomic<unsigned long> target_pid_{0};
    std::thread worker_;
    mutable std::mutex sample_mutex_;
    mutable PresentSample latest_;          // guarded by sample_mutex_
    mutable uint64_t last_present_qpc_ = 0; // reader-side drain state (Latest())
    mutable int64_t qpc_freq_ = 0;
    void* impl_ = nullptr; // opaque SessionImpl* (PMTraceConsumer + TraceSession)
};

} // namespace exosnap::diagnostics
