#pragma once

// Live recording-pipeline diagnostics (DIAGNOSTICS-LIVE-PIPELINE-R1).
//
// Immutable, UI-free typed telemetry snapshot for the active recording pipeline.
// Produced by SessionStatsCollector on a worker thread from the per-session
// PipelineDiagnosticsAggregator and delivered via DiagnosticsCallback. Every value
// is sourced from the subsystem that owns it; metrics that cannot be measured are
// marked Unavailable rather than reported as a fake zero.
//
// This header intentionally depends only on codec_types.h so that recorder_session.h
// can include it to declare SetDiagnosticsCallback without creating an include cycle.

#include "codec_types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace recorder_core {

// Whether a metric is backed by a real measurement this session.
enum class MetricAvailability : uint8_t {
    Available,
    Unavailable,
};

// Deterministic, evidence-based bottleneck classification.
enum class PipelineBottleneck : uint8_t {
    None,         // recording, no sustained constraint detected
    Capture,      // capture cannot keep up / capture-side loss
    Compositor,   // composition approaching frame budget
    VideoEncoder, // encoder backlog / latency rising
    Audio,        // audio drops / queue pressure
    Muxer,        // mux queue rising, writes healthy
    Disk,         // write latency/throughput insufficient
    Unknown,      // insufficient evidence
};

// Lightweight measurable health state.
enum class PipelineHealth : uint8_t {
    Idle, // no active recording
    Good,
    Warning,
    Critical,
    Unavailable, // diagnostics instrumentation unavailable
};

// Recording lifecycle as reflected in diagnostics presentation.
enum class DiagnosticsLifecycle : uint8_t {
    Idle,
    Initializing, // checking / countdown / preparing
    Recording,
    Paused,
    Stopping, // finalizing
    Completed,
    Failed,
};

// Capture source classification (derived from target kind + crop).
enum class CaptureSourceType : uint8_t {
    Unknown,
    Display,
    Window,
    Region,
};

// Mirror of SplitTriggerSource for the snapshot (kept local to avoid a heavy
// include cycle with recorder_session.h). None == no split has occurred yet.
enum class DiagnosticsSplitTrigger : uint8_t {
    None,
    AutomaticDuration,
    AutomaticSize,
    ManualButton,
    Hotkey,
};

struct CaptureDiagnostics {
    double target_fps = 0.0;
    double actual_fps = 0.0;                  // emitted frames Δ / elapsed Δ between publishes
    uint64_t frames_captured = 0;             // frames the capture backend produced (true)
    uint64_t frames_emitted = 0;              // frames handed to the encoder (CFR output rate)
    uint64_t frames_dropped_coalesced = 0;    // a newer frame replaced an unconsumed one
    uint64_t frames_dropped_cfr = 0;          // a scheduled tick produced no frame
    uint64_t frames_dropped_backpressure = 0; // encoder input slots were all in flight
    uint64_t frames_duplicated = 0;           // CFR hold of the last real frame
    double frame_interval_ms = 0.0;
    MetricAvailability interval_observed = MetricAvailability::Unavailable; // true only on VFR
    CaptureSourceType source_type = CaptureSourceType::Unknown;
    bool source_loss = false;

    [[nodiscard]] uint64_t frames_dropped_total() const noexcept {
        return frames_dropped_coalesced + frames_dropped_cfr + frames_dropped_backpressure;
    }
};

struct CompositorDiagnostics {
    bool active = false; // an overlay/cursor composition pass actually ran
    double latest_ms = 0.0;
    double average_ms = 0.0;
    double peak_ms = 0.0;
    uint64_t frames_composed = 0;
    // CPU command-submission time, NOT GPU execution time. GPU execution timing is
    // Unavailable (no timestamp-query infrastructure; no synchronous readback allowed).
};

struct EncoderDiagnostics {
    double latest_ms = 0.0; // submit -> bitstream-ready (synchronous lock) latency
    double average_ms = 0.0;
    double peak_ms = 0.0;
    double output_fps = 0.0; // encoded packets Δ / elapsed Δ
    uint64_t frames_submitted = 0;
    uint64_t frames_encoded = 0;
    uint64_t backlog = 0;          // submitted - encoded (NVENC need-more-input buffering)
    uint64_t forced_keyframes = 0; // split-driven forced IDRs (not periodic GOP)
    VideoCodec codec = VideoCodec::Av1Nvenc;
    uint32_t width = 0;
    uint32_t height = 0;
    bool cfr = true;
};

struct AudioDiagnostics {
    bool active = false;
    uint64_t packets_encoded = 0;
    uint64_t bytes_encoded = 0;
    uint32_t queue_depth = 0; // WASAPI pending-frame proxy
    uint32_t queue_peak = 0;
    uint64_t discontinuities = 0; // coarse drop signal (data-discontinuity flags)
    MetricAvailability discontinuity_availability = MetricAvailability::Available;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    AudioCodec codec = AudioCodec::Opus;
    uint32_t track_count = 0;
};

struct QueueDiagnostics {
    uint32_t current_depth = 0;
    uint32_t peak_depth = 0;
    uint32_t capacity = 0; // 0 == unbounded / not fixed
    bool bounded = false;
    uint64_t dropped_items = 0;
    MetricAvailability availability = MetricAvailability::Available;
};

struct MuxDiagnostics {
    uint64_t packets_processed = 0;
    uint64_t bytes_written = 0; // at the filesystem write boundary (cluster render)
    double throughput_mib_s = 0.0;
    double latest_write_ms = 0.0; // write-call latency (buffered stdio), not physical media
    double average_write_ms = 0.0;
    double peak_write_ms = 0.0;
    uint32_t current_segment_index = 0; // 0-based
    uint32_t segment_count = 0;
    uint64_t finalizations = 0;
    double latest_finalize_ms = 0.0;
    uint64_t split_transitions = 0;
    uint64_t failures = 0;
    // Streaming Matroska reorder window (bounded). Segment-local.
    uint32_t reorder_packets = 0;
    uint32_t reorder_packets_peak = 0;
    uint64_t reorder_bytes = 0;
    uint64_t reorder_bytes_peak = 0;
    MetricAvailability availability = MetricAvailability::Available; // Unavailable for MP4
};

struct DiskDiagnostics {
    uint64_t bytes_written = 0;
    double throughput_mib_s = 0.0;
    double latest_write_ms = 0.0; // write-call latency, NOT physical disk latency
    double average_write_ms = 0.0;
    double peak_write_ms = 0.0;
    std::string output_target; // drive / root only (privacy-safe)
    uint64_t write_failures = 0;
    MetricAvailability latency_availability = MetricAvailability::Available;
};

struct SplitDiagnostics {
    bool split_supported = false; // container can split (Matroska/WebM yes, MP4 no)
    uint32_t current_segment = 0; // 1-based for display (0 == none yet)
    uint32_t completed_segments = 0;
    bool split_pending = false;
    DiagnosticsSplitTrigger last_trigger = DiagnosticsSplitTrigger::None;
    double last_finalize_ms = 0.0;
    uint64_t split_failures = 0;
    double seconds_until_auto_split = -1.0; // <0 == not scheduled / unavailable
    MetricAvailability availability = MetricAvailability::Available;
};

// Canonical immutable diagnostics snapshot.
struct RecordingDiagnosticsSnapshot {
    uint64_t session_generation = 0;
    DiagnosticsLifecycle lifecycle = DiagnosticsLifecycle::Idle;
    double elapsed_seconds = 0.0;
    bool valid = false; // false == idle / no data; UI shows neutral, never fake zeros

    CaptureDiagnostics capture;
    CompositorDiagnostics compositor;
    EncoderDiagnostics video_encoder;
    AudioDiagnostics audio;
    QueueDiagnostics video_queue; // post-encode mux queue (unbounded by design)
    QueueDiagnostics audio_queue; // audio premux (bounded)
    MuxDiagnostics mux;
    DiskDiagnostics disk;
    SplitDiagnostics split;

    // A/V synchronization drift measured from stream PTS.
    // Positive = audio leads video; negative = video leads audio.
    double av_drift_ms = 0.0;
    MetricAvailability av_drift_availability = MetricAvailability::Unavailable;

    // Estimated seconds until the output drive fills at current sustained throughput.
    // Negative means unavailable (throughput unknown or free space not provided).
    double disk_fill_eta_seconds = -1.0;

    PipelineBottleneck bottleneck = PipelineBottleneck::None;
    std::string bottleneck_reason;
    PipelineHealth health = PipelineHealth::Idle;
};

// Delivered approximately every 200 ms (5 Hz) while diagnostics are active, from an
// internal worker thread. Implementations must be thread-safe. Must be set before Record().
using DiagnosticsCallback = std::function<void(const RecordingDiagnosticsSnapshot&)>;

// Monotonic session-generation guard. A stale recording's late snapshot (lower
// generation) can never update a newer recording's view. Used by the app sink on the
// UI thread; the generation is stamped per Record() by the engine.
class DiagnosticsSessionGuard {
  public:
    [[nodiscard]] bool Accept(const RecordingDiagnosticsSnapshot& snapshot) noexcept {
        if (snapshot.session_generation < max_generation_) {
            return false;
        }
        max_generation_ = snapshot.session_generation;
        return true;
    }

    [[nodiscard]] uint64_t max_generation() const noexcept {
        return max_generation_;
    }

    void Reset() noexcept {
        max_generation_ = 0;
    }

  private:
    uint64_t max_generation_ = 0;
};

// --- Stable, locale-independent names for logging and tests (UI-free) ---------

[[nodiscard]] constexpr const char* ToString(PipelineBottleneck b) noexcept {
    switch (b) {
    case PipelineBottleneck::None:
        return "None";
    case PipelineBottleneck::Capture:
        return "Capture";
    case PipelineBottleneck::Compositor:
        return "Compositor";
    case PipelineBottleneck::VideoEncoder:
        return "VideoEncoder";
    case PipelineBottleneck::Audio:
        return "Audio";
    case PipelineBottleneck::Muxer:
        return "Muxer";
    case PipelineBottleneck::Disk:
        return "Disk";
    case PipelineBottleneck::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(PipelineHealth h) noexcept {
    switch (h) {
    case PipelineHealth::Idle:
        return "Idle";
    case PipelineHealth::Good:
        return "Good";
    case PipelineHealth::Warning:
        return "Warning";
    case PipelineHealth::Critical:
        return "Critical";
    case PipelineHealth::Unavailable:
        return "Unavailable";
    }
    return "Unavailable";
}

} // namespace recorder_core
