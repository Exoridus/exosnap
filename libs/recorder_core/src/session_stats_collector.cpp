#include "session_stats_collector.h"

#include <recorder_core/logging/logging.h>

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace recorder_core {

namespace {

// Stable, locale-independent tokens for the perf log records.
const char* CodecToken(VideoCodec c) noexcept {
    switch (c) {
    case VideoCodec::Av1Nvenc:
        return "av1";
    case VideoCodec::H264Nvenc:
        return "h264";
    case VideoCodec::HevcNvenc:
        return "hevc";
    }
    return "unknown";
}

const char* PresetToken(NvencPreset p) noexcept {
    switch (p) {
    case NvencPreset::P1:
        return "P1";
    case NvencPreset::P2:
        return "P2";
    case NvencPreset::P3:
        return "P3";
    case NvencPreset::P4:
        return "P4";
    case NvencPreset::P5:
        return "P5";
    case NvencPreset::P6:
        return "P6";
    case NvencPreset::P7:
        return "P7";
    }
    return "P?";
}

const char* RateControlToken(RateControlMode m) noexcept {
    switch (m) {
    case RateControlMode::ConstantQuality:
        return "cq";
    case RateControlMode::VariableBitrate:
        return "vbr";
    case RateControlMode::ConstantBitrate:
        return "cbr";
    case RateControlMode::Lossless:
        return "lossless";
    }
    return "unknown";
}

// Fixed-precision double -> string (locale-independent, no trailing exponent).
std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

std::string U64(uint64_t v) {
    return std::to_string(v);
}

// Comma-separated bucket counts for the whole-session histogram serialisation.
std::string Buckets(const std::array<uint64_t, LatencyHistogram::kBucketCount>& b) {
    std::string out;
    out.reserve(b.size() * 4);
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += std::to_string(b[i]);
    }
    return out;
}

} // namespace

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
    constexpr int kStatsEveryNTicks = 8;        // 8 × 33 ms ≈ 264 ms
    constexpr int kDiagnosticsEveryNTicks = 6;  // 6 × 33 ms ≈ 198 ms ≈ 5 Hz
    constexpr int kPerfRecordEveryNTicks = 300; // 300 × 33 ms ≈ 9.9 s (a multiple of 6)
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
        // The periodic perf log record (~10 s) reuses this same snapshot, so it is also
        // produced when a perf record is due even if no UI callback is registered.
        const bool diag_tick = (tick % kDiagnosticsEveryNTicks == 0);
        const bool perf_tick = (tick % kPerfRecordEveryNTicks == 0);
        if ((m_state.diagnostics_callback && diag_tick) || perf_tick) {
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
            if (m_state.diagnostics_callback && diag_tick) {
                m_state.diagnostics_callback(diagnostics);
            }

            // Periodic perf-window record (log-only). One structured line every ~10 s
            // while recording, so a session leaves a time series of encode-latency and
            // frame-time percentiles that the analysis script can chart. Only recording
            // ticks carry meaningful pipeline timing.
            if (perf_tick && lifecycle == DiagnosticsLifecycle::Recording) {
                const PerfWindowSample p = m_state.diagnostics.SamplePerfWindow(now);
                const EncoderDiagnostics& enc = diagnostics.video_encoder;
                const VideoTimingDiagnostics& vt = diagnostics.video_timing;
                const EncoderInitInfo& init = diagnostics.encoder_init;

                std::string res =
                    std::to_string(stats_copy.output_size.width) + "x" + std::to_string(stats_copy.output_size.height) +
                    "@" +
                    std::to_string(stats_copy.frame_rate_den > 0 ? stats_copy.frame_rate_num / stats_copy.frame_rate_den
                                                                 : stats_copy.frame_rate_num);

                const std::vector<logging::LogField> fields = {
                    {"perf_schema", "1"},
                    {"elapsed_s", Num(elapsed)},
                    {"encode_p50_ms", Num(p.encode_p50_ms)},
                    {"encode_p95_ms", Num(p.encode_p95_ms)},
                    {"encode_p99_ms", Num(p.encode_p99_ms)},
                    {"encode_peak_ms", Num(p.encode_peak_ms)},
                    {"submit_p50_ms", Num(p.submit_p50_ms)},
                    {"submit_p99_ms", Num(p.submit_p99_ms)},
                    {"tick_p50_ms", Num(p.tick_p50_ms)},
                    {"tick_p95_ms", Num(p.tick_p95_ms)},
                    {"tick_p99_ms", Num(p.tick_p99_ms)},
                    {"tick_peak_ms", Num(p.tick_peak_ms)},
                    {"tick_budget_ms", Num(vt.budget_ms)},
                    {"output_fps", Num(enc.output_fps)},
                    {"backlog", U64(enc.backlog)},
                    {"dropped_backpressure", U64(p.dropped_backpressure)},
                    {"slot_stalls", U64(p.slot_stalls)},
                    {"preset", PresetToken(init.preset)},
                    {"codec", CodecToken(stats_copy.video_codec)},
                    {"resolution", res},
                };
                logging::log(logging::LogLevel::Info, "perf", "video-pipeline-window",
                             std::span<const logging::LogField>(fields.data(), fields.size()));
            }
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

    EmitSessionPerfSummary();
}

void SessionStatsCollector::EmitSessionPerfSummary() {
    // One whole-session distribution record at collector shutdown. The bucket
    // counts are the authoritative gate data; the analysis script recomputes
    // percentiles from them (the live windows are noisy over ~2 s). Frames after
    // this point (EOS drain) are not represented — documented and irrelevant to
    // the steady-state question this measures.
    const PerfSessionSummary sum = m_state.diagnostics.BuildPerfSummary();
    if (sum.encode_count == 0 && sum.tick_count == 0) {
        return; // never recorded (e.g. an aborted / no-frame session)
    }

    SessionStats stats_copy;
    {
        std::lock_guard lk(m_state.stats_mutex);
        stats_copy = m_state.stats;
    }
    double duration_skew_ms = 0.0;
    if (stats_copy.video_duration_ns > 0 && stats_copy.audio_duration_ns > 0) {
        const double vd = static_cast<double>(stats_copy.video_duration_ns) / 1e6;
        const double ad = static_cast<double>(stats_copy.audio_duration_ns) / 1e6;
        duration_skew_ms = (vd > ad) ? (vd - ad) : (ad - vd);
    }

    std::string res =
        std::to_string(stats_copy.output_size.width) + "x" + std::to_string(stats_copy.output_size.height) + "@" +
        std::to_string(stats_copy.frame_rate_den > 0 ? stats_copy.frame_rate_num / stats_copy.frame_rate_den
                                                     : stats_copy.frame_rate_num);

    const std::vector<logging::LogField> fields = {
        {"perf_schema", "1"},
        {"hist_lo_ms", Num(LatencyHistogram::kLoMs)},
        {"hist_hi_ms", Num(LatencyHistogram::kHiMs)},
        {"hist_buckets", std::to_string(LatencyHistogram::kBucketCount)},
        {"encode_count", U64(sum.encode_count)},
        {"encode_p50_ms", Num(sum.encode_p50_ms)},
        {"encode_p99_ms", Num(sum.encode_p99_ms)},
        {"encode_hist", Buckets(sum.encode_buckets)},
        {"submit_count", U64(sum.submit_count)},
        {"submit_hist", Buckets(sum.submit_buckets)},
        {"tick_count", U64(sum.tick_count)},
        {"tick_p50_ms", Num(sum.tick_p50_ms)},
        {"tick_p99_ms", Num(sum.tick_p99_ms)},
        {"tick_hist", Buckets(sum.tick_buckets)},
        {"frames_emitted", U64(stats_copy.video_frames_captured)},
        {"frames_encoded", U64(stats_copy.encoded_video_packets)},
        {"frames_duplicated", U64(stats_copy.duplicated_video_frames)},
        {"frames_dropped_or_skipped", U64(stats_copy.dropped_or_skipped_video_frames)},
        {"dropped_backpressure", U64(sum.dropped_backpressure)},
        {"slot_stalls", U64(sum.slot_stalls)},
        {"duration_skew_ms", Num(duration_skew_ms)},
        {"preset", PresetToken(sum.encoder_init.preset)},
        {"rc_mode", RateControlToken(sum.encoder_init.rc_mode)},
        {"gop_length", U64(sum.encoder_init.gop_length)},
        {"codec", CodecToken(stats_copy.video_codec)},
        {"resolution", res},
    };
    logging::log(logging::LogLevel::Info, "perf", "session-perf-summary",
                 std::span<const logging::LogField>(fields.data(), fields.size()));
}

} // namespace recorder_core
