#pragma once

#include "RecommendationEngine.h" // DpcLatencyReading

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace exosnap::diagnostics {

// What a consumer of DPC/ISR latency is allowed to know: one reading, and nothing
// about how it was obtained. The Diagnostics host layer holds this rather than the
// concrete class so the wiring stays assertable without a real kernel trace --
// opening one needs elevation, and a unit test may not tear a machine-wide named
// ETW session out from under a running ExoSnap.
class IDpcLatencyProvider {
  public:
    virtual ~IDpcLatencyProvider() = default;
    // `available == false` means "not being measured right now" -- never a zero
    // reading to be reported as if it had been measured.
    [[nodiscard]] virtual DpcLatencyReading Read() const = 0;
};

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
//   * Stop() snapshots, stops the session + CloseTrace (to unblock ProcessTrace), waits
//     for the worker with a ceiling and abandons it past that, then resets impl_; the
//     destructor calls Stop().
//
// Threading: Start()/Stop()/Read() are called from the GUI thread. The only blocking
// call in the class -- ProcessTrace -- runs on worker_, and Read() takes a short
// accumulator lock, so the Diagnostics refresh never waits on the kernel.
class DpcLatencyProvider final : public IDpcLatencyProvider {
  public:
    DpcLatencyProvider();
    ~DpcLatencyProvider() override;
    DpcLatencyProvider(const DpcLatencyProvider&) = delete;
    DpcLatencyProvider& operator=(const DpcLatencyProvider&) = delete;

    [[nodiscard]] bool Start();
    void Stop();
    // True while a consumer is actually consuming, not merely "Start() succeeded".
    // Same reasoning as PresentMonEtwSession::IsOpen(): a ProcessTrace that returned
    // for any other reason -- the named session stopped by another process, a driver
    // reset, an ETW buffer error -- must not leave the provider claiming to measure.
    [[nodiscard]] bool IsOpen() const;
    // Snapshot of the accumulated reading. available == false until at least one
    // DPC/ISR event has been measured, and again as soon as the trace stops being
    // consumed: a peak that is no longer being updated is not a measurement.
    [[nodiscard]] DpcLatencyReading Read() const override;

  private:
    // Runs ProcessTrace (blocking) on worker_. Static, and handed the session as a
    // shared owner rather than reaching it through the provider: Stop() may abandon
    // this thread, and a member function would still be dereferencing a destroyed
    // provider on its way to the very pointer that keeps the session alive.
    static void ConsumeLoop(std::shared_ptr<void> session);

    std::atomic<bool> open_{false};
    std::thread worker_;
    // Guards the impl_ pointer lifetime only. The accumulators inside SessionImpl have
    // their own internal lock (written by the ETW callback on the worker thread, read by
    // Read()). Snapshotting impl_ under this lock keeps SessionImpl alive across Stop().
    mutable std::mutex impl_mutex_;
    std::shared_ptr<void> impl_;
};

} // namespace exosnap::diagnostics
