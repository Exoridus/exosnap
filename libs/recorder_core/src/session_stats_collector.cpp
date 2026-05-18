#include "session_stats_collector.h"

namespace recorder_core {

SessionStatsCollector::SessionStatsCollector(SessionState& state) : m_state(state) {
}

SessionStatsCollector::~SessionStatsCollector() {
    Stop();
}

void SessionStatsCollector::Start() {
    m_start_time = std::chrono::steady_clock::now();
    m_stop.store(false);
    m_thread = std::thread([this] { Run(); });
}

void SessionStatsCollector::Stop() {
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
}

void SessionStatsCollector::Run() {
    constexpr auto kInterval = std::chrono::milliseconds(250);

    while (!m_stop.load()) {
        std::this_thread::sleep_for(kInterval);
        if (m_stop.load())
            break;

        // Snapshot stats
        SessionStats snapshot;
        {
            std::lock_guard lk(m_state.stats_mutex);
            snapshot = m_state.stats;
        }

        // Update elapsed
        auto now = std::chrono::steady_clock::now();
        snapshot.elapsed_seconds = std::chrono::duration<double>(now - m_start_time).count();

        // Compute skew
        if (snapshot.video_duration_ns > 0 && snapshot.audio_duration_ns > 0) {
            double vd = static_cast<double>(snapshot.video_duration_ns) / 1e6; // ms
            double ad = static_cast<double>(snapshot.audio_duration_ns) / 1e6; // ms
            snapshot.duration_skew_ms = (vd > ad) ? (vd - ad) : (ad - vd);
        }

        // Invoke callback (if set)
        if (m_state.stats_callback) {
            m_state.stats_callback(snapshot);
        }
    }
}

} // namespace recorder_core
