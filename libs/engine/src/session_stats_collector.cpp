#include "session_stats_collector.h"

#include <exosnap/engine/logging/logging.h>

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace exosnap::engine {

namespace {

// Stable, locale-independent tokens for the perf log records.
const char* CodecToken(VideoCodec c) noexcept {
    switch (c) {
    case VideoCodec::Av1:
        return "av1";
    case VideoCodec::H264:
        return "h264";
    case VideoCodec::Hevc:
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

// Append one measured stage's live-window statistics as `<prefix>_{mean,p50,p95,
// p99,max}_ms` so every stage reads uniformly in the perf log.
void AddStageWindow(std::vector<logging::LogField>& fields, const std::string& prefix, const StageWindowStats& s) {
    fields.push_back({prefix + "_mean_ms", Num(s.mean_ms)});
    fields.push_back({prefix + "_p50_ms", Num(s.p50_ms)});
    fields.push_back({prefix + "_p95_ms", Num(s.p95_ms)});
    fields.push_back({prefix + "_p99_ms", Num(s.p99_ms)});
    fields.push_back({prefix + "_max_ms", Num(s.max_ms)});
}

// Append one stage's whole-session distribution: count, p50/p95/p99 and the
// authoritative bucket array.
void AddStageSummary(std::vector<logging::LogField>& fields, const std::string& prefix,
                     const StageHistogramSummary& s) {
    fields.push_back({prefix + "_count", U64(s.count)});
    fields.push_back({prefix + "_p50_ms", Num(s.p50_ms)});
    fields.push_back({prefix + "_p95_ms", Num(s.p95_ms)});
    fields.push_back({prefix + "_p99_ms", Num(s.p99_ms)});
    fields.push_back({prefix + "_hist", Buckets(s.buckets)});
}

} // namespace

SessionStatsCollector::SessionStatsCollector(SessionState& state) : m_state(state) {
}

SessionStatsCollector::~SessionStatsCollector() {
    Stop();
}

void SessionStatsCollector::Start() {
    m_start_time = std::chrono::steady_clock::now();
    m_paused_since.reset();
    m_state.paused_ns.store(0);
    m_stop.store(false);
    m_thread = std::thread([this] { Run(); });
}

void SessionStatsCollector::Stop() {
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
    // A pause still open when the session ends was never added to paused_ns
    // (the loop above only books a window when it sees it close), so a failure
    // raised while paused counted the whole paused stretch as recorded time.
    // The window ends where the capture did, not where this join returned.
    if (m_paused_since) {
        const auto capture_end_ns = m_state.capture_end_ns.load();
        const auto end = capture_end_ns > 0
                             ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(capture_end_ns))
                             : std::chrono::steady_clock::now();
        if (end > *m_paused_since) {
            m_state.paused_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - *m_paused_since).count());
        }
        m_paused_since.reset();
    }
}

void SessionStatsCollector::AccumulatePause(std::chrono::steady_clock::time_point now) {
    const bool paused = m_state.pause_requested.load();
    if (paused && !m_paused_since) {
        m_paused_since = now;
        return;
    }
    if (!paused && m_paused_since) {
        const auto held = now - *m_paused_since;
        m_state.paused_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(held).count());
        m_paused_since.reset();
    }
}

double CapturedSeconds(std::chrono::nanoseconds since_start, std::chrono::nanoseconds paused_total,
                       std::chrono::nanoseconds open_pause) noexcept {
    const double seconds = std::chrono::duration<double>(since_start - paused_total - open_pause).count();
    return seconds > 0.0 ? seconds : 0.0;
}

double SessionStatsCollector::CapturedSecondsAt(std::chrono::steady_clock::time_point now) const {
    const auto open = m_paused_since ? std::chrono::duration_cast<std::chrono::nanoseconds>(now - *m_paused_since)
                                     : std::chrono::nanoseconds::zero();
    return CapturedSeconds(std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start_time),
                           std::chrono::nanoseconds(m_state.paused_ns.load()), open);
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
        AccumulatePause(std::chrono::steady_clock::now());

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
            const double elapsed = CapturedSecondsAt(now);
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

                std::vector<logging::LogField> fields = {
                    {"perf_schema", "2"},
                    {"elapsed_s", Num(elapsed)},
                    {"tick_budget_ms", Num(vt.budget_ms)},
                    {"output_fps", Num(enc.output_fps)},
                    {"backlog", U64(enc.backlog)},
                };
                // Every measured stage, mean/p50/p95/p99/max, CPU-submission and
                // real GPU-execution kept distinct by name.
                AddStageWindow(fields, "acquire", p.acquire);
                AddStageWindow(fields, "composition_cpu", p.composition_cpu);
                AddStageWindow(fields, "composition_gpu", p.composition_gpu);
                AddStageWindow(fields, "hdr_tonemap_gpu", p.hdr_tonemap_gpu);
                AddStageWindow(fields, "rgb_to_yuv_cpu", p.rgb_to_yuv_cpu);
                AddStageWindow(fields, "rgb_to_yuv_gpu", p.rgb_to_yuv_gpu);
                AddStageWindow(fields, "encode_submit", p.encode_submit);
                AddStageWindow(fields, "encode_latency", p.encode_latency);
                AddStageWindow(fields, "tick", p.tick);
                AddStageWindow(fields, "webcam_convert", p.webcam_convert);
                AddStageWindow(fields, "webcam_upload_gpu", p.webcam_upload_gpu);
                AddStageWindow(fields, "preview_copy", p.preview_copy);
                AddStageWindow(fields, "mux_process", p.mux_process);
                AddStageWindow(fields, "mux_queue_delay", p.mux_queue_delay);
                fields.push_back({"dropped_coalesced", U64(p.dropped_coalesced)});
                fields.push_back({"dropped_cfr", U64(p.dropped_cfr)});
                fields.push_back({"dropped_backpressure", U64(p.dropped_backpressure)});
                fields.push_back({"dropped_processing_failure", U64(p.dropped_processing_failure)});
                fields.push_back({"duplicated_frames", U64(p.duplicated_frames)});
                fields.push_back({"slot_stalls", U64(p.slot_stalls)});
                fields.push_back({"queue_saturation_events", U64(p.queue_saturation_events)});
                fields.push_back({"preset", PresetToken(init.preset)});
                fields.push_back({"codec", CodecToken(stats_copy.video_codec)});
                fields.push_back({"resolution", res});
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
            snapshot.elapsed_seconds = CapturedSecondsAt(now);

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
    if (sum.encode_latency.count == 0 && sum.tick.count == 0) {
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

    std::vector<logging::LogField> fields = {
        {"perf_schema", "2"},
        {"hist_lo_ms", Num(LatencyHistogram::kLoMs)},
        {"hist_hi_ms", Num(LatencyHistogram::kHiMs)},
        {"hist_buckets", std::to_string(LatencyHistogram::kBucketCount)},
    };
    // Every measured stage's whole-session distribution (the authoritative gate
    // data). CPU-submission and real GPU-execution stages are named distinctly.
    AddStageSummary(fields, "acquire", sum.acquire);
    AddStageSummary(fields, "composition_cpu", sum.composition_cpu);
    AddStageSummary(fields, "composition_gpu", sum.composition_gpu);
    AddStageSummary(fields, "hdr_tonemap_gpu", sum.hdr_tonemap_gpu);
    AddStageSummary(fields, "rgb_to_yuv_cpu", sum.rgb_to_yuv_cpu);
    AddStageSummary(fields, "rgb_to_yuv_gpu", sum.rgb_to_yuv_gpu);
    AddStageSummary(fields, "encode_submit", sum.encode_submit);
    AddStageSummary(fields, "encode_latency", sum.encode_latency);
    AddStageSummary(fields, "tick", sum.tick);
    AddStageSummary(fields, "webcam_convert", sum.webcam_convert);
    AddStageSummary(fields, "webcam_upload_gpu", sum.webcam_upload_gpu);
    AddStageSummary(fields, "preview_copy", sum.preview_copy);
    AddStageSummary(fields, "mux_process", sum.mux_process);
    AddStageSummary(fields, "mux_queue_delay", sum.mux_queue_delay);
    fields.push_back({"frames_emitted", U64(stats_copy.video_frames_captured)});
    fields.push_back({"frames_encoded", U64(stats_copy.encoded_video_packets)});
    fields.push_back({"frames_duplicated", U64(sum.duplicated_frames)});
    fields.push_back({"frames_dropped_or_skipped", U64(stats_copy.dropped_or_skipped_video_frames)});
    fields.push_back({"dropped_coalesced", U64(sum.dropped_coalesced)});
    fields.push_back({"dropped_cfr", U64(sum.dropped_cfr)});
    fields.push_back({"dropped_backpressure", U64(sum.dropped_backpressure)});
    fields.push_back({"dropped_processing_failure", U64(sum.dropped_processing_failure)});
    fields.push_back({"slot_stalls", U64(sum.slot_stalls)});
    fields.push_back({"queue_saturation_events", U64(sum.queue_saturation_events)});
    fields.push_back({"duration_skew_ms", Num(duration_skew_ms)});
    fields.push_back({"preset", PresetToken(sum.encoder_init.preset)});
    fields.push_back({"rc_mode", RateControlToken(sum.encoder_init.rc_mode)});
    fields.push_back({"gop_length", U64(sum.encoder_init.gop_length)});
    fields.push_back({"codec", CodecToken(stats_copy.video_codec)});
    fields.push_back({"resolution", res});
    logging::log(logging::LogLevel::Info, "perf", "session-perf-summary",
                 std::span<const logging::LogField>(fields.data(), fields.size()));
}

} // namespace exosnap::engine
