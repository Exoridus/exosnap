#pragma once

#include "RecommendationEngine.h" // DpcLatencyReading

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace exosnap::diagnostics {

// Owns a real-time kernel system-trace ETW session (DPC + ISR + image-load) and a
// consumer worker. Read() returns the accumulated max/avg DPC+ISR latency plus a
// best-effort attribution of the worst-offending kernel driver. Requires elevation;
// Start() returns false (graceful) when the session cannot be opened.
//
// This mirrors the reviewed lifecycle/threading shape of PresentMonEtwSession:
//   * impl_ is an opaque shared_ptr<void> (no Win32/ETW type leaks into the header);
//   * the worker thread runs the blocking ProcessTrace;
//   * Read()/Stop() snapshot impl_ under impl_mutex_ before deref so a concurrent
//     Stop() cannot free the SessionImpl mid-read;
//   * Stop() snapshots, stops the session + CloseTrace (to unblock ProcessTrace),
//     joins the worker, then resets impl_; the destructor calls Stop().
class DpcLatencyProvider {
  public:
    DpcLatencyProvider();
    ~DpcLatencyProvider();
    DpcLatencyProvider(const DpcLatencyProvider&) = delete;
    DpcLatencyProvider& operator=(const DpcLatencyProvider&) = delete;

    [[nodiscard]] bool Start();
    void Stop();
    [[nodiscard]] bool IsOpen() const {
        return open_.load(std::memory_order_acquire);
    }
    // Snapshot of the accumulated reading. available == false until at least one
    // DPC/ISR event has been measured (or when the session is not open).
    [[nodiscard]] DpcLatencyReading Read() const;

  private:
    void ConsumeLoop(); // runs ProcessTrace (blocking) on worker_

    std::atomic<bool> open_{false};
    std::thread worker_;
    // Guards the impl_ pointer lifetime only. The accumulators inside SessionImpl have
    // their own internal lock (written by the ETW callback on the worker thread, read by
    // Read()). Snapshotting impl_ under this lock keeps SessionImpl alive across Stop().
    mutable std::mutex impl_mutex_;
    std::shared_ptr<void> impl_;
};

} // namespace exosnap::diagnostics
