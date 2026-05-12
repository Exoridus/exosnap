#pragma once

// Stats timer: fires the StatsCallback approximately every 250 ms.
// Runs on a background thread owned by this object.

#include "session_internal.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace recorder_core {

class SessionStatsCollector {
public:
    explicit SessionStatsCollector(SessionState& state);
    ~SessionStatsCollector();

    SessionStatsCollector(const SessionStatsCollector&)            = delete;
    SessionStatsCollector& operator=(const SessionStatsCollector&) = delete;

    // Start the stats timer thread.
    void Start();

    // Stop the stats timer thread (blocks until the thread exits).
    void Stop();

private:
    void Run();

    SessionState& m_state;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::chrono::steady_clock::time_point m_start_time;
};

} // namespace recorder_core
