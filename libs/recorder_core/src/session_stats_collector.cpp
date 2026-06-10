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
    constexpr auto kMeterInterval = std::chrono::milliseconds(33);
    constexpr int kStatsEveryNTicks = 8;       // 8 × 33 ms ≈ 264 ms
    constexpr int kDiagnosticsEveryNTicks = 6; // 6 × 33 ms ≈ 198 ms ≈ 5 Hz
    int tick = 0;

    while (!m_stop.load()) {
        std::this_thread::sleep_for(kMeterInterval);
        if (m_stop.load())
            break;
        ++tick;

        // High-cadence meter snapshot (~30 Hz)
        if (m_state.meter_callback) {
            MeterSnapshot meter;
            {
                std::lock_guard lk(m_state.stats_mutex);
                meter.per_track_rms = m_state.stats.per_track_rms;
            }
            m_state.meter_callback(meter);
        }

        // Live pipeline diagnostics, bounded cadence (~5 Hz). Built from the worker-fed
        // aggregator plus a stats copy; lifecycle derived from the session stop/pause tokens.
        if (m_state.diagnostics_callback && (tick % kDiagnosticsEveryNTicks == 0)) {
            SessionStats stats_copy;
            {
                std::lock_guard lk(m_state.stats_mutex);
                stats_copy = m_state.stats;
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - m_start_time).count();
            stats_copy.elapsed_seconds = elapsed;

            const DiagnosticsLifecycle lifecycle = m_state.stop_requested.load()    ? DiagnosticsLifecycle::Stopping
                                                   : m_state.pause_requested.load() ? DiagnosticsLifecycle::Paused
                                                                                    : DiagnosticsLifecycle::Recording;
            const RecordingDiagnosticsSnapshot diagnostics =
                m_state.diagnostics.BuildSnapshot(now, stats_copy, lifecycle, elapsed);
            m_state.diagnostics_callback(diagnostics);
        }

        // Full stats every 8 ticks (~264 ms)
        if (tick % kStatsEveryNTicks == 0) {
            SessionStats snapshot;
            {
                std::lock_guard lk(m_state.stats_mutex);
                snapshot = m_state.stats;
            }

            auto now = std::chrono::steady_clock::now();
            snapshot.elapsed_seconds = std::chrono::duration<double>(now - m_start_time).count();

            if (snapshot.video_duration_ns > 0 && snapshot.audio_duration_ns > 0) {
                double vd = static_cast<double>(snapshot.video_duration_ns) / 1e6; // ms
                double ad = static_cast<double>(snapshot.audio_duration_ns) / 1e6; // ms
                snapshot.duration_skew_ms = (vd > ad) ? (vd - ad) : (ad - vd);
            }

            if (m_state.stats_callback) {
                m_state.stats_callback(snapshot);
            }
        }
    }
}

} // namespace recorder_core
