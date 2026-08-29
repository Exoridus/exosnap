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

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace exosnap::engine {

// Whether a metric is backed by a real measurement this session.
enum class MetricAvailability : uint8_t {
    Available,
    Unavailable,
    // Sampled, and known to be wrong: the measurement's own preconditions were
    // violated (see PlausibleDriftBoundMs). Distinct from Unavailable on
    // purpose -- "nothing measured yet" invites waiting, "measured and invalid"
    // must not be read as a number at all.
    Faulted,
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

// Presentation mode of the captured source (present/tearing diagnostics, ADR 0033).
// Mirror of app::diagnostics::PresentMode, kept local so this engine header
// does not depend on the app-layer PresentProvider.h (layering: app → core only).
enum class PresentMode : uint8_t {
    Unknown,
    Composed,
    IndependentFlip,
    ExclusiveFullscreen,
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
    double actual_fps = 0.0;                        // emitted frames Δ / elapsed Δ between publishes
    uint64_t frames_captured = 0;                   // frames the capture backend produced (true)
    uint64_t frames_emitted = 0;                    // frames handed to the encoder (CFR output rate)
    uint64_t frames_dropped_coalesced = 0;          // a newer frame replaced an unconsumed one
    uint64_t frames_dropped_cfr = 0;                // a scheduled tick had nothing to encode yet (benign pacing)
    uint64_t frames_dropped_backpressure = 0;       // encoder input slots were all in flight
    uint64_t frames_dropped_processing_failure = 0; // a frame was available and its conversion failed
    uint64_t frames_duplicated = 0;                 // CFR hold of the last real frame
    double frame_interval_ms = 0.0;
    MetricAvailability interval_observed = MetricAvailability::Unavailable; // true only on VFR
    CaptureSourceType source_type = CaptureSourceType::Unknown;
    bool source_loss = false;

    // Present cadence (VRR/CFR judder correlation, v0.8.0 / ADR 0033). DXGI Output
    // Duplication only: derived from DXGI_OUTDUPL_FRAME_INFO.LastPresentTime (QPC) deltas
    // and AccumulatedFrames. Unavailable for WGC (Window/Region) capture, which exposes no
    // present timestamp, and during warm-up / before enough samples accumulate.
    double source_present_interval_ms = 0.0; // mean inter-present interval over the rolling window
    double source_present_jitter_ms = 0.0;   // peak-minus-average present interval (irregular-pacing proxy)
    double source_coalesce_ratio = 1.0;      // mean AccumulatedFrames per acquire (>1 == presents coalesced)
    MetricAvailability present_cadence_availability = MetricAvailability::Unavailable;

    // Present mode + tearing (PresentMon ETW present-diagnostics, ADR 0033). Elevation-
    // and opt-in-gated; Unavailable until the in-process PresentMon consumer is vendored
    // and a real present has been observed (never a fabricated Composed/zero).
    PresentMode source_present_mode = PresentMode::Unknown;
    bool source_tearing = false;
    MetricAvailability present_mode_availability = MetricAvailability::Unavailable;

    // Acquire+copy CPU duration (Source Capture card). steady_clock bracket around the
    // backend acquire/drain; NOT GPU time. Unavailable until the first sample this session.
    double acquire_latest_ms = 0.0;
    double acquire_average_ms = 0.0;
    double acquire_peak_ms = 0.0;
    MetricAvailability acquire_availability = MetricAvailability::Unavailable;

    [[nodiscard]] uint64_t frames_dropped_total() const noexcept {
        return frames_dropped_coalesced + frames_dropped_cfr + frames_dropped_backpressure +
               frames_dropped_processing_failure;
    }

    // "Problematic" drops: the drops that cost real picture. Excludes the two benign
    // categories -- frames_dropped_coalesced (the source is simply faster than the CFR
    // target, e.g. a 144 Hz display recorded at 60 fps) and frames_dropped_cfr (a
    // scheduled tick before the first frame exists, which resolves itself).
    //
    // This is the single definition of a "real" drop, shared by the internal
    // bottleneck/health classification AND every user-facing drop surface (report
    // card, review panel, live tile, dropped-frames notification), so the two can
    // never disagree about whether a session dropped frames.
    [[nodiscard]] uint64_t frames_dropped_problem() const noexcept {
        return frames_dropped_processing_failure + frames_dropped_backpressure;
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

    // VideoProcessorBlt (BGRA->NV12) CPU submission duration, folded into the Compositor
    // card. steady_clock bracket around VideoProcessorBlt; NOT GPU execution time.
    double vpblt_latest_ms = 0.0;
    double vpblt_average_ms = 0.0;
    double vpblt_peak_ms = 0.0;
    MetricAvailability vpblt_availability = MetricAvailability::Unavailable;

    // A requested webcam PiP / cursor overlay is not being recorded in the active
    // mode (native HDR10 from an already-PQ 10-bit desktop composites nothing —
    // the surface is non-linear). Calm notice, not a fault.
    bool overlay_omitted = false;
};

struct EncoderDiagnostics {
    double latest_ms = 0.0; // submit -> bitstream-ready (true per-frame) latency
    double average_ms = 0.0;
    double peak_ms = 0.0;
    // Rolling-window (≈2 s) percentiles of the same submit->ready encode latency.
    // Engine-internal in this stage: consumed by tests and the periodic perf log
    // record, not yet surfaced on any diagnostics card (the visibility decision is
    // deferred until the measurement campaign shows whether it is product-relevant).
    double p50_ms = 0.0;
    double p99_ms = 0.0;
    double output_fps = 0.0; // encoded packets Δ / elapsed Δ
    uint64_t frames_submitted = 0;
    uint64_t frames_encoded = 0;
    uint64_t backlog = 0;          // submitted - encoded (NVENC need-more-input buffering)
    uint64_t forced_keyframes = 0; // split-driven forced IDRs (not periodic GOP)
    // Order/keyframe validation mismatch counters. Non-zero indicates the
    // driver's actual outputTimeStamp echo / pictureType diverged from the
    // submission-side FIFO assignment / GOP-phase prediction at least once.
    //
    // keyframe_prediction_mismatches is warn-only and counts every occurrence.
    //
    // output_ts_mismatches is structurally always 0: an output-timestamp
    // mismatch aborts the encode before the packet is filled in, so the
    // EncodedVideoPacket flag this counter is fed from never reaches the video
    // thread as true. The counter is kept for symmetry with the packet flag and
    // so the wiring is already in place should the check ever be relaxed back to
    // a warning — read the encode failure, not this counter, to find out that a
    // mismatch happened.
    uint64_t output_ts_mismatches = 0;
    uint64_t keyframe_prediction_mismatches = 0;
    VideoCodec codec = VideoCodec::Av1;
    uint32_t width = 0;
    uint32_t height = 0;
    bool cfr = true;
};

// Immutable encoder initialization parameters, captured once when the encoder is
// configured and carried unchanged on every snapshot thereafter. Plain data only —
// no NVENC types leak to the app layer (ADR: engine stays UI-agnostic). `valid` is
// false until the encoder has been configured (e.g. a failure before configure).
struct EncoderInitInfo {
    bool valid = false;
    VideoCodec codec = VideoCodec::Av1;
    NvencPreset preset = NvencPreset::P4;
    RateControlMode rc_mode = RateControlMode::ConstantQuality;
    uint32_t target_bitrate_kbps = 0; // averageBitRate (0 for pure CQ)
    uint32_t max_bitrate_kbps = 0;    // maxBitRate
    uint32_t cq = 0;                  // constant-quality target (CQ mode)
    uint32_t gop_length = 0;          // frames between IDRs
    uint32_t bframes = 0;             // B-frames per GOP (this pipeline: 0)
    uint32_t lookahead_frames = 0;    // rate-control lookahead depth (this pipeline: 0)
    bool temporal_aq = false;
    bool spatial_aq = false;
    BitDepth bit_depth = BitDepth::Bit8;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    bool color_full_range = false;
    HdrMode hdr_mode = HdrMode::Off;
};

// Whole-tick video-thread frame time: composite + tonemap + VP-Blt + encode +
// mux-queue wait, measured per emitted frame from the start of the tick body to
// after the packet is routed. This is the metric checked against the frame budget
// (16.67 ms @ 60 fps). Engine-internal in this stage (tests + periodic perf log
// record); not surfaced on any diagnostics card yet. Percentiles are rolling-window
// (≈2 s); Unavailable until the first emitted frame this session.
struct VideoTimingDiagnostics {
    double tick_p50_ms = 0.0;
    double tick_p99_ms = 0.0;
    double tick_peak_ms = 0.0;
    double budget_ms = 0.0; // 1000 / target_fps
    MetricAvailability availability = MetricAvailability::Unavailable;
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
    // Device hot-swap health (ADR 0046). degraded_sources = capture sources
    // across all audio tracks whose endpoint is currently lost and contributing
    // honest silence (the recording keeps running; the source reactivates when
    // the device returns). source_degraded == degraded_sources > 0. A calm,
    // measured live notice — never a blocker.
    uint32_t degraded_sources = 0;
    uint32_t degraded_source_kinds = 0;
    bool source_degraded = false;
    // Latched post-flight fact: at least one audio capture source was lost
    // mid-recording and degraded to honest silence at some point this session,
    // even if every source is healthy again by the final snapshot. Passed through
    // from SessionStats; there is no per-event history behind it.
    bool source_degraded_occurred = false;
    // Resampler tail flushed at stop, per track (index = audio track id, bounded
    // by CodecPrivateData::kMaxAudioTracks). Passed through from SessionStats and
    // therefore only populated on the terminal snapshot, after the audio workers
    // have drained. undrained > 0 means captured audio was dropped. The counters
    // are meaningful only where resampler_drain_recorded is true — a track that
    // never reached its drain (failed session, join timeout, no resample context)
    // leaves them at 0, which is not a measurement.
    std::array<bool, 3> resampler_drain_recorded{};
    std::array<uint64_t, 3> resampler_drained_frames{};
    std::array<uint64_t, 3> resampler_undrained_frames{};
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

    // Mux drain-loop CPU processing duration (Muxer card). steady_clock bracket around
    // the per-iteration queue drain; Unavailable until the first batch is processed.
    double process_latest_ms = 0.0;
    double process_average_ms = 0.0;
    double process_peak_ms = 0.0;
    MetricAvailability process_availability = MetricAvailability::Unavailable;
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
// How one acquire ended. The distinction matters because only ONE of these is a
// dropped frame.
//
// A keyed mutex does not follow the Win32 mutex rule that WAIT_ABANDONED hands
// the caller ownership to release: for IDXGIKeyedMutex::AcquireSync it means the
// shared surface and the mutex are no longer in a consistent state, and both are
// to be released and recreated. Treating it as another contention drop leaves a
// transport that can never recover, and releasing on it would be worse still.
enum class PreviewAcquireOutcome {
    Acquired,  // S_OK
    Contended, // WAIT_TIMEOUT: the other side holds it, drop this frame
    Abandoned, // WAIT_ABANDONED: this transport generation is poisoned
    Failed,    // any other HRESULT
};

// The WYSIWYG preview tap, counted at every branch it can leave through.
//
// The tap is observation-only and non-blocking by contract: a preview frame is
// dropped rather than allowed to stall the encode. That makes a permanently
// dead preview indistinguishable from a healthy one from the encoder's side —
// capture, composition, encode and mux stay perfect either way. These counters
// exist so "the preview showed nothing" resolves to ONE branch instead of five
// equally consistent stories.
//
// Read them as a funnel: each number is a subset of the one above it.
struct PreviewTapDiagnostics {
    // Ticks that reached the tap with a composited frame to offer. Zero means the
    // tap never ran at all — no consumer registered, the tap disabled for this
    // session, or the session never produced a frame the tap could see.
    uint64_t frames_seen = 0;
    // Ticks the publish gate let through (the tap throttles to ~30 Hz, so this is
    // legitimately far below frames_seen).
    uint64_t gate_passes = 0;
    // True once the shared texture exists and its handle has been handed to the
    // consumer. A permanent false with frames_seen > 0 is a creation failure.
    bool shared_texture_ready = false;
    // Publish attempts, and how they ended.
    // attempts == successes + contended + abandoned + failed.
    //
    // `contended` is the expected, harmless outcome of a 0 ms acquire and costs
    // one frame. `abandoned` is not: the keyed mutex reports the shared surface
    // as inconsistent, and this transport generation cannot recover on its own.
    // `release_failures` counts copies that were made and then failed to hand the
    // key over, which strands the mutex on the producer key.
    uint64_t publish_attempts = 0;
    uint64_t publish_successes = 0;
    uint64_t publish_mutex_misses = 0; // WAIT_TIMEOUT
    uint64_t publish_abandoned = 0;
    uint64_t publish_failures = 0;
    uint64_t publish_release_failures = 0;
    // Publish edges actually emitted to the consumer. Below publish_successes
    // means the notification path, not the GPU transport, is what broke.
    uint64_t published_edges = 0;
};

struct RecordingDiagnosticsSnapshot {
    uint64_t session_generation = 0;
    DiagnosticsLifecycle lifecycle = DiagnosticsLifecycle::Idle;
    double elapsed_seconds = 0.0;
    bool valid = false; // false == idle / no data; UI shows neutral, never fake zeros

    CaptureDiagnostics capture;
    CompositorDiagnostics compositor;
    EncoderDiagnostics video_encoder;
    VideoTimingDiagnostics video_timing;
    AudioDiagnostics audio;
    QueueDiagnostics video_queue; // post-encode mux queue (unbounded by design)
    QueueDiagnostics audio_queue; // audio premux (bounded)
    MuxDiagnostics mux;
    DiskDiagnostics disk;
    SplitDiagnostics split;
    PreviewTapDiagnostics preview_tap;

    // Retained-frame reuse counters (spec section 13)
    uint64_t screen_generation_changes = 0;
    uint64_t webcam_generation_changes = 0;
    uint64_t cursor_only_capture_events_ignored = 0;
    uint64_t phase_ring_cursor_only_events_ignored = 0;
    uint64_t full_compositions = 0;
    uint64_t reused_yuv_frames = 0;
    uint64_t yuv_slot_copies = 0;
    uint64_t yuv_slot_copies_skipped = 0;

    // A/V synchronization drift as it actually lands in the file — the RESIDUAL
    // after clock slaving. The audio device clock (WASAPI device-position/QPC
    // pairs) is measured against the QPC timeline video frames are paced on
    // (raw drift, av_drift_raw_ms); clock slaving then pulls the audio output
    // timeline back onto that axis, and this field is what remains (raw drift
    // minus the applied compensation). Positive = audio leads video. Unavailable
    // until a device-backed audio track has reported timing; a multi-source
    // merge mixes several device clocks and does not report. Deliberately NOT the
    // difference of the two pipelines' most recent output PTS — that only sees
    // encoder/queue latency. Division of labour: av_drift_ms / av_drift_raw_ms
    // are the clocks; duration_skew_ms below is encoder starvation.
    double av_drift_ms = 0.0;
    MetricAvailability av_drift_availability = MetricAvailability::Unavailable;

    // Raw measured device-clock-vs-QPC drift of the same track av_drift_ms is
    // taken from (before clock-slaving compensation). Equals av_drift_ms when
    // slaving has not engaged; diverges from it as the controller corrects.
    double av_drift_raw_ms = 0.0;

    // Current clock-slaving compensation rate of that same track, in ppm. 0.0 =
    // not compensating (below the engage threshold, or between quantized steps).
    double clock_slaving_ppm = 0.0;

    // True while any audio track's clock slaving has engaged (is actively pulling
    // its output timeline onto the QPC axis). Informational, not an alarm.
    bool clock_slaving_active = false;

    // Peak |av_drift_ms| observed this session (running maximum of the residual
    // magnitude). Accumulated in the engine aggregator so the live UI and the
    // on-disk session report read one authoritative value rather than each
    // maintaining its own. Unavailable until av_drift has been measured at least
    // once.
    double peak_av_drift_ms = 0.0;
    MetricAvailability peak_av_drift_availability = MetricAvailability::Unavailable;

    // Encoder initialization parameters, captured once at configure time.
    EncoderInitInfo encoder_init;

    // Total media-duration skew: |video media time - audio media time| (ms). Unlike
    // av_drift_ms (an instantaneous PTS lead/lag), this is the accumulated difference
    // in how much video vs audio the file holds — the signal that a starving encoder
    // is compressing the video timeline. Unavailable until both streams have duration.
    double duration_skew_ms = 0.0;
    MetricAvailability duration_skew_availability = MetricAvailability::Unavailable;

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

} // namespace exosnap::engine
