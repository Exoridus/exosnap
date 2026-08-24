#pragma once

// Stats timer: fires MeterCallback at ~30 Hz (33 ms) and StatsCallback at ~264 ms (every 8 meter ticks).
// Runs on a background thread owned by this object.

#include "session_internal.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace exosnap::engine {

class SessionStatsCollector {
  public:
    explicit SessionStatsCollector(SessionState& state);
    ~SessionStatsCollector();

    SessionStatsCollector(const SessionStatsCollector&) = delete;
    SessionStatsCollector& operator=(const SessionStatsCollector&) = delete;

    // Start the stats timer thread.
    void Start();

    // Stop the stats timer thread (blocks until the thread exits).
    void Stop();

  private:
    void Run();

    // Emit the whole-session perf distribution to the engine log when the
    // collector loop exits (log-only; no-op if no frames were measured).
    void EmitSessionPerfSummary();

    SessionState& m_state;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::chrono::steady_clock::time_point m_start_time;
};

} // namespace exosnap::engine
