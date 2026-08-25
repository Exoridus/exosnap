#pragma once

// Stats timer: fires MeterCallback at ~30 Hz (33 ms) and StatsCallback at ~264 ms (every 8 meter ticks).
// Runs on a background thread owned by this object.

#include "session_internal.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

namespace exosnap::engine {

// Wall-clock time since a recording started, minus everything it spent paused.
//
// Free and pure so it can be asserted without a clock: the arithmetic is the
// part that is easy to get wrong, and a test that had to pause a real session
// for a real second would be the slowest in the suite.
//
// `open_pause` is a pause that has not ended yet and is therefore not in
// `paused_total`. Leaving it out would let the clock run on through a pause and
// then jump backwards when it resumed.
//
// Never negative: the three inputs are sampled without a lock between them, and
// a clock that briefly reads below zero would print as a negative duration.
[[nodiscard]] double CapturedSeconds(std::chrono::nanoseconds since_start, std::chrono::nanoseconds paused_total,
                                     std::chrono::nanoseconds open_pause) noexcept;

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

    // This session's captured seconds at `now`, from the pure function above.
    [[nodiscard]] double CapturedSecondsAt(std::chrono::steady_clock::time_point now) const;

    // Observes `pause_requested` and accumulates the time between its edges.
    void AccumulatePause(std::chrono::steady_clock::time_point now);

    // Emit the whole-session perf distribution to the engine log when the
    // collector loop exits (log-only; no-op if no frames were measured).
    void EmitSessionPerfSummary();

    SessionState& m_state;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::chrono::steady_clock::time_point m_start_time;
    // Set while a pause is open. The accumulated total lives in SessionState,
    // because the session end reports it too.
    std::optional<std::chrono::steady_clock::time_point> m_paused_since;
};

} // namespace exosnap::engine
