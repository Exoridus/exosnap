#pragma once

// Internal per-session aggregator for live pipeline diagnostics.
// Worker threads feed cheap counters/timers; the stats collector calls BuildSnapshot
// (~5 Hz) to produce an immutable RecordingDiagnosticsSnapshot. Time is injected
// (steady_clock::time_point) so aggregation/cadence/expiry are deterministically testable
// without sleeps. Not part of the public API.

#include "perf_histogram.h"

#include <exosnap/engine/pipeline_diagnostics.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/session_stats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace exosnap::engine {

// Fixed-capacity ring of timestamped samples with a bounded time horizon.
// One allocation at construction; Add() never allocates. Compute() ignores samples
// older than `horizon` so peaks expire and averages stay bounded.
class RollingTimeWindow {
  public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    struct Aggregate {
        std::size_t count = 0;
        double latest = 0.0;
        double average = 0.0;
        double peak = 0.0;
    };

    explicit RollingTimeWindow(std::size_t capacity = 192,
                               std::chrono::milliseconds horizon = std::chrono::milliseconds(2000))
        : buf_(capacity == 0 ? 1 : capacity), horizon_(horizon) {
    }

    void Add(time_point t, double value) noexcept {
        buf_[head_] = Sample{t, value};
        head_ = (head_ + 1) % buf_.size();
        if (size_ < buf_.size()) {
            ++size_;
        }
    }

    [[nodiscard]] Aggregate Compute(time_point now) const noexcept {
        Aggregate agg;
        const time_point cutoff = now - horizon_;
        double sum = 0.0;
        time_point newest{};
        bool have_newest = false;
        for (std::size_t i = 0; i < size_; ++i) {
            // Walk most-recent first.
            const std::size_t idx = (head_ + buf_.size() - 1 - i) % buf_.size();
            const Sample& s = buf_[idx];
            if (s.t < cutoff) {
                break; // older entries are even older (ring is time-ordered)
            }
            if (!have_newest || s.t > newest) {
                newest = s.t;
                agg.latest = s.v;
                have_newest = true;
            }
            sum += s.v;
            if (agg.count == 0 || s.v > agg.peak) {
                agg.peak = s.v;
            }
            ++agg.count;
        }
        if (agg.count > 0) {
            agg.average = sum / static_cast<double>(agg.count);
        }
        return agg;
    }

    // Exact quantile (q in [0,1]) over the samples still inside the horizon.
    // Copies the live window (<= capacity) into a scratch buffer and partitions
    // it — cheap at 256 samples / 5 Hz. Returns 0 when the window is empty. Used
    // for the live encode/tick p50/p99 snapshot fields; the whole-session gate
    // data lives in the mergeable LatencyHistogram instead.
    [[nodiscard]] double Percentile(time_point now, double q) const noexcept {
        const time_point cutoff = now - horizon_;
        std::array<double, 512> scratch;
        std::size_t n = 0;
        for (std::size_t i = 0; i < size_ && n < scratch.size(); ++i) {
            const std::size_t idx = (head_ + buf_.size() - 1 - i) % buf_.size();
            const Sample& s = buf_[idx];
            if (s.t < cutoff) {
                break; // ring is time-ordered; older entries are even older
            }
            scratch[n++] = s.v;
        }
        if (n == 0) {
            return 0.0;
        }
        if (q < 0.0) {
            q = 0.0;
        }
        if (q > 1.0) {
            q = 1.0;
        }
        // Nearest-rank on a 0-based sorted index, clamped to the last element.
        auto rank = static_cast<std::size_t>(q * static_cast<double>(n - 1) + 0.5);
        if (rank >= n) {
            rank = n - 1;
        }
        std::nth_element(scratch.begin(), scratch.begin() + rank, scratch.begin() + n);
        return scratch[rank];
    }

    void Clear() noexcept {
        head_ = 0;
        size_ = 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return buf_.size();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

  private:
    struct Sample {
        time_point t{};
        double v = 0.0;
    };
    std::vector<Sample> buf_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::chrono::nanoseconds horizon_;
};

// Centralized, testable thresholds for bottleneck/health classification.
struct DiagnosticsThresholds {
    int sustain_samples = 3;         // consecutive publishes a condition must hold
    double capture_fps_ratio = 0.85; // actual/target below this == capture pressure
    double compositor_budget_ratio = 0.8;
    double encoder_budget_ratio = 0.8;
    uint64_t encoder_backlog = 2;         // submitted - encoded sustained
    uint32_t mux_queue_warn = 8;          // mux (video) queue depth
    double disk_write_ms_warn = 8.0;      // write-call latency ms
    double audio_queue_warn_ratio = 0.8;  // fraction of audio-queue capacity
    double queue_critical_ratio = 0.9;    // fraction of a bounded queue == critical
    double warmup_seconds = 1.0;          // below this, evidence is insufficient (Unknown)
    double duration_skew_warn_ms = 250.0; // video/audio media-duration skew that reads as a warning
};

// Static, per-session configuration not carried in SessionStats.
struct DiagnosticsStaticConfig {
    CaptureSourceType source_type = CaptureSourceType::Unknown;
    bool split_supported = false; // container can split (Matroska/WebM)
    bool auto_split = false;
    double auto_split_seconds = 0.0;
    std::string output_target; // drive / root only
    uint32_t audio_track_count = 0;
    bool audio_present = false;
    uint32_t video_queue_capacity = 0; // 0 == unbounded
    uint32_t audio_queue_capacity = 0; // premux bound (bounded)
};

// ---- Perf measurement records (log-only; engine-internal) -----------------
// One consistent set of live-window statistics for a single measured stage. Every
// stage — CPU-submission timing and true GPU-execution timing alike — is reported
// through this same shape so the perf log and the analysis script treat them
// uniformly. All values are milliseconds; `samples` is the count still inside the
// rolling horizon. Empty window == all zeros.
struct StageWindowStats {
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    std::size_t samples = 0;
};

// Derive the uniform live-window stats for one stage from its rolling window.
// Pure: percentiles are computed only when the window is non-empty.
[[nodiscard]] inline StageWindowStats SampleStage(const RollingTimeWindow& w,
                                                  RollingTimeWindow::time_point now) noexcept {
    const RollingTimeWindow::Aggregate a = w.Compute(now);
    StageWindowStats s;
    s.mean_ms = a.average;
    s.max_ms = a.peak;
    s.samples = a.count;
    if (a.count > 0) {
        s.p50_ms = w.Percentile(now, 0.50);
        s.p95_ms = w.Percentile(now, 0.95);
        s.p99_ms = w.Percentile(now, 0.99);
    }
    return s;
}

// Whole-session distribution for one stage: the mergeable bucket histogram (the
// authoritative gate data) plus a count and convenience percentiles recomputed
// from it.
struct StageHistogramSummary {
    std::array<uint64_t, LatencyHistogram::kBucketCount> buckets{};
    uint64_t count = 0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

// Fold one whole-session histogram into its summary shape.
[[nodiscard]] inline StageHistogramSummary SummarizeStage(const LatencyHistogram& h) noexcept {
    StageHistogramSummary s;
    s.buckets = h.BucketCounts();
    s.count = h.count();
    s.p50_ms = h.Quantile(0.50);
    s.p95_ms = h.Quantile(0.95);
    s.p99_ms = h.Quantile(0.99);
    return s;
}

// Rolling-window (≈2 s) sample emitted periodically to the engine log so a
// recording produces a time series of per-stage percentiles. CPU-submission
// timing (how long the CPU spent recording/issuing the commands) and true
// GPU-execution timing (how long the GPU spent running them) are kept as
// separate, explicitly named stages so the two are never conflated.
struct PerfWindowSample {
    // Capture
    StageWindowStats acquire; // acquire + copy (CPU)
    // Composition / colour conversion
    StageWindowStats composition_cpu; // GpuCompositor submit (CPU)
    StageWindowStats composition_gpu; // composite pass execution (GPU)
    StageWindowStats hdr_tonemap_gpu; // scRGB->SDR tone-map pass execution (GPU)
    StageWindowStats rgb_to_yuv_cpu;  // VideoProcessorBlt submit (CPU)
    StageWindowStats rgb_to_yuv_gpu;  // VideoProcessorBlt execution (GPU)
    // Encode
    StageWindowStats encode_submit;  // EncodeFrame call cost (CPU)
    StageWindowStats encode_latency; // submit -> bitstream-available
    StageWindowStats tick;           // whole video-thread tick
    // Webcam overlay
    StageWindowStats webcam_convert;    // source -> BGRA conversion (CPU)
    StageWindowStats webcam_upload_gpu; // overlay texture upload (GPU)
    // Preview
    StageWindowStats preview_copy; // WYSIWYG preview tap copy (CPU)
    // Mux / disk
    StageWindowStats mux_process;     // drain-loop work (CPU)
    StageWindowStats mux_queue_delay; // enqueue -> dequeue wait in the mux queue

    // Cumulative counters this session.
    uint64_t dropped_coalesced = 0;
    uint64_t dropped_cfr = 0;
    uint64_t dropped_backpressure = 0;
    uint64_t dropped_processing_failure = 0;
    uint64_t dropped_ring_eviction = 0;
    uint64_t duplicated_frames = 0;       // CFR duplicate output frames
    uint64_t slot_stalls = 0;             // subset of backpressure
    uint64_t queue_saturation_events = 0; // rising-edge crossings of a queue's critical threshold
};

// Whole-session distribution captured once at session end. The bucket arrays are
// the authoritative gate data (the live windows are noisy over ~2 s); the script
// recomputes percentiles from them. Percentiles here are provided for convenience.
struct PerfSessionSummary {
    StageHistogramSummary acquire;
    StageHistogramSummary composition_cpu;
    StageHistogramSummary composition_gpu;
    StageHistogramSummary hdr_tonemap_gpu;
    StageHistogramSummary rgb_to_yuv_cpu;
    StageHistogramSummary rgb_to_yuv_gpu;
    StageHistogramSummary encode_submit;
    StageHistogramSummary encode_latency;
    StageHistogramSummary tick;
    StageHistogramSummary webcam_convert;
    StageHistogramSummary webcam_upload_gpu;
    StageHistogramSummary preview_copy;
    StageHistogramSummary mux_process;
    StageHistogramSummary mux_queue_delay;

    uint64_t dropped_coalesced = 0;
    uint64_t dropped_cfr = 0;
    uint64_t dropped_backpressure = 0;
    uint64_t dropped_processing_failure = 0;
    uint64_t dropped_ring_eviction = 0;
    uint64_t duplicated_frames = 0;
    uint64_t slot_stalls = 0;
    uint64_t queue_saturation_events = 0;
    EncoderInitInfo encoder_init;
};

// Build the static config from a RecorderConfig (UI-free).
[[nodiscard]] DiagnosticsStaticConfig MakeDiagnosticsStaticConfig(const RecorderConfig& config);

// Map the engine's SplitTriggerSource onto the snapshot enum.
[[nodiscard]] DiagnosticsSplitTrigger ToDiagnosticsSplitTrigger(SplitTriggerSource src) noexcept;

class PipelineDiagnosticsAggregator {
  public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    PipelineDiagnosticsAggregator();

    PipelineDiagnosticsAggregator(const PipelineDiagnosticsAggregator&) = delete;
    PipelineDiagnosticsAggregator& operator=(const PipelineDiagnosticsAggregator&) = delete;

    // Begin a new logical recording session: resets every counter, window, rate
    // baseline and classifier state, and stamps the session generation.
    void Reset(uint64_t generation, const DiagnosticsStaticConfig& cfg);

    void SetThresholds(const DiagnosticsThresholds& thresholds);

    // ---- worker-thread inputs (thread-safe, never throw) -------------------
    // Capture (VideoThread)
    void OnFrameCaptured() noexcept;                                  // a frame the backend actually produced
    void OnFrameDroppedCoalesced() noexcept;                          // newer frame replaced an unconsumed one
    void OnFrameDroppedRingEviction() noexcept;                       // captured, never emitted, overwritten
    void OnFrameDroppedCfr() noexcept;                                // scheduled tick had nothing to encode yet
    void OnFrameDroppedBackpressure() noexcept;                       // encoder input slots all in flight
    void OnFrameDroppedProcessingFailure() noexcept;                  // a frame was there and its conversion failed
    void OnObservedFrameInterval(time_point now, double ms) noexcept; // VFR only
    // Present cadence (DXGI OD only): inter-present interval (ms) from LastPresentTime QPC
    // deltas plus the coalesced-update count (AccumulatedFrames). O(1), allocation-free.
    void OnSourcePresentInterval(time_point now, double interval_ms, uint32_t accumulated_frames) noexcept;
    // Compositor (VideoThread) — CPU submission time only
    void OnCompositorSubmit(time_point now, double ms, bool pass_ran) noexcept;
    // Capture-card live wiring (0.8.0): cheap CPU-timing brackets (steady_clock).
    void OnAcquireLatency(time_point now, double ms) noexcept; // acquire+copy (Source Capture)
    void OnVpbltSubmit(time_point now, double ms) noexcept;    // VideoProcessorBlt submit (CPU)
    void OnMuxLatency(time_point now, double ms) noexcept;     // mux drain loop (Muxer)

    // ---- GPU-execution-time inputs (real GPU work, not CPU submission) ------
    // Fed from resolved D3D11 timestamp-query spans (see gpu_timestamp_profiler.h).
    // Each is a distinct pass so composition, tone mapping and the RGB->YUV blit
    // never collapse into one number, and each is explicitly named *_gpu so it is
    // never confused with the CPU-submission window of the same pass. Callers pass
    // only resolved, non-disjoint spans; a not-ready or disjoint frame is simply
    // dropped upstream and never reaches these.
    void OnCompositionGpuTime(time_point now, double ms) noexcept;
    void OnHdrTonemapGpuTime(time_point now, double ms) noexcept;
    void OnRgbToYuvGpuTime(time_point now, double ms) noexcept;
    void OnWebcamUploadGpuTime(time_point now, double ms) noexcept;

    // ---- Additional CPU-timed stages ---------------------------------------
    void OnWebcamConvert(time_point now, double ms) noexcept; // source -> BGRA conversion
    void OnPreviewCopy(time_point now, double ms) noexcept;   // WYSIWYG preview tap copy
    // The preview tap's own branch counters. Separate from OnPreviewCopy, which
    // times the copy and therefore only ever sees the successful path.
    void OnPreviewTapFrameSeen() noexcept;
    void OnPreviewTapGatePass() noexcept;
    void OnPreviewTapSharedTextureReady() noexcept;
    void OnPreviewTapPublish(PreviewAcquireOutcome outcome, bool released_ok) noexcept;
    void OnPreviewTapPublishedEdge() noexcept;
    // Time a packet actually waited in the post-encode mux queue between being
    // enqueued and being dequeued by the drain loop (distinct from OnMuxLatency,
    // which times the drain loop's own work once it holds the packet).
    void OnMuxQueueDelay(time_point now, double ms) noexcept;
    // A CFR duplicate output frame was emitted (the last real frame re-submitted
    // to fill a scheduled output slot that produced no new source frame).
    void OnFrameDuplicated() noexcept;
    // Retained-frame reuse (CV-RETAIN-001..004, CV-CURSOR-001..003, CV-PACING-001).
    void OnScreenGenerationChanged() noexcept;
    void OnWebcamGenerationChanged() noexcept;
    void OnCursorOnlyCaptureEventIgnored() noexcept;   // Ignorable-classified OD acquire
    void OnPhaseRingCursorOnlyEventIgnored() noexcept; // CursorOnly-classified: no ring entry made
    void OnFullComposition() noexcept;                 // a real composite+convert ran
    void OnReusedYuvFrame() noexcept;                  // CFR duplicate reused the cached YUV frame
    void OnYuvSlotCopy() noexcept;                     // duplicate path actually copied into the slot
    void OnYuvSlotCopySkipped() noexcept;              // duplicate path found the slot already current
    // Encoder (VideoThread)
    void OnEncodeSubmitted() noexcept;
    // True submit -> bitstream-available latency for one frame (from
    // EncodedVideoPacket::encode_latency_ms; reported only when available). Feeds
    // the live window and the whole-session histogram.
    void OnEncodeLatency(time_point now, double ms) noexcept;
    // CPU cost of the EncodeFrame call itself (the former call-site bracket). In
    // the synchronous path this ≈ encode latency at P1-P4; the difference at
    // P5-P7 (and, later, in async mode) is the pipelining head-room. Separate
    // window/histogram so the two are never conflated.
    void OnEncodeSubmitCost(time_point now, double ms) noexcept;
    // Whole-tick video-thread frame time for one emitted frame.
    void OnVideoTickTime(time_point now, double ms) noexcept;
    // An encoder input slot was unavailable this tick (a subset of the
    // backpressure drops; tracked separately so the analysis can split slot
    // stalls from sustained-lag resync skips).
    void OnSlotStall() noexcept;
    void OnForcedKeyframe() noexcept;
    // Order/keyframe validation mismatch counters. A keyframe-prediction
    // mismatch is warn-only (legal, just off-cadence SEI/OBU placement); an
    // output-timestamp mismatch is fatal and aborts the encode before a
    // packet is ever produced, so in practice this counter never exceeds 1.
    // Fed per-packet from EncodedVideoPacket::output_ts_mismatch /
    // keyframe_prediction_mismatch; the encoder itself has no aggregator
    // reference, so these arrive via the same packet-field transport as
    // OnEncodeLatency.
    void OnOutputTsMismatch() noexcept;
    void OnKeyframePredictionMismatch() noexcept;
    // Encoder init parameters, captured once by the encoder at configure time and
    // carried unchanged on every subsequent snapshot.
    void SetEncoderInitInfo(const EncoderInitInfo& info) noexcept;
    // Audio (AudioThread)
    void SetAudioFormat(uint32_t sample_rate, uint32_t channels) noexcept;
    void OnAudioQueueDepth(uint32_t depth) noexcept;
    // gap_frames is the measured length of the outage, in source frames.
    void OnAudioDiscontinuity(uint32_t gap_frames) noexcept;
    // Device hot-swap health for one audio track (ADR 0046): how many of the
    // track's capture sources are currently degraded (endpoint lost, silent) out
    // of its total. Level-based (the current state, not an event), reported each
    // drain iteration; the snapshot sums across tracks. track_id is bounded by
    // CodecPrivateData::kMaxAudioTracks.
    void OnAudioSourceHealth(uint32_t track_id, uint32_t degraded_sources, uint32_t total_sources,
                             uint32_t degraded_source_kinds = 0, bool endpoint_in_use = false) noexcept;
    // Queues
    void OnVideoQueueDepth(uint32_t depth) noexcept;  // post-encode mux queue
    void OnAudioPremuxDepth(uint32_t depth) noexcept; // bounded premux
    // Mux / Disk (MuxThread)
    void OnMuxPacket(uint64_t bytes) noexcept;
    void OnDiskWrite(time_point now, double ms, uint64_t bytes) noexcept; // filesystem boundary
    void OnSegmentOpened(uint32_t index) noexcept;
    void OnSegmentFinalized(double ms, bool succeeded) noexcept;
    void OnSplitTransition(DiagnosticsSplitTrigger trigger) noexcept;
    void OnMuxFailure() noexcept;
    void OnReorderWindow(uint32_t packets, uint32_t peak_packets, uint64_t bytes, uint64_t peak_bytes) noexcept;
    void SetSplitPending(bool pending) noexcept;
    // A/V clock slaving state for one audio track (AudioThread). raw_drift_ms is
    // the measured device-vs-QPC drift (AudioClockDriftEstimator; positive =
    // audio leads video); residual_ms is what remains after the applied
    // compensation (equals raw before slaving engages); applied_ppm is the
    // current compensation rate (0 = not compensating). Tracks without an
    // attributable device clock (multi-source merges) never report.
    void OnAudioClockSlaving(uint32_t track_id, double raw_drift_ms, double residual_ms, double applied_ppm,
                             bool measurement_faulted = false) noexcept;
    void OnAudioClockSlavingSaturated(uint32_t track_id) noexcept;
    // Free-space poll for disk-fill ETA (called from the stats collector at ~5 Hz)
    void UpdateFreeDiskBytes(uint64_t free_bytes) noexcept;

    // ---- collector-thread publish -----------------------------------------
    [[nodiscard]] RecordingDiagnosticsSnapshot BuildSnapshot(time_point now, const SessionStats& stats,
                                                             DiagnosticsLifecycle lifecycle, double elapsed_seconds);

    // Perf log support (collector thread). Both take/return copies under mutex_.
    [[nodiscard]] PerfWindowSample SamplePerfWindow(time_point now) const;
    [[nodiscard]] PerfSessionSummary BuildPerfSummary() const;

    [[nodiscard]] uint64_t generation() const noexcept;

  private:
    PipelineBottleneck Classify(const RecordingDiagnosticsSnapshot& s, bool recording, std::string& reason,
                                PipelineHealth& health);

    mutable std::mutex mutex_;
    DiagnosticsThresholds thresholds_;
    DiagnosticsStaticConfig cfg_;
    uint64_t generation_ = 0;

    // Capture counters
    uint64_t frames_captured_ = 0;
    uint64_t dropped_coalesced_ = 0;
    uint64_t dropped_cfr_ = 0;
    uint64_t dropped_backpressure_ = 0;
    uint64_t dropped_processing_failure_ = 0;
    uint64_t dropped_ring_eviction_ = 0;
    // Capture progress watch (CaptureDiagnostics::capture_starved).
    uint64_t last_progress_frames_ = 0;
    std::chrono::steady_clock::time_point last_progress_time_{};
    bool have_progress_time_ = false;
    bool interval_observed_ = false;
    RollingTimeWindow interval_window_{256, std::chrono::milliseconds(2000)};

    // Present cadence (DXGI OD only): inter-present interval and coalescing proxy windows.
    bool present_observed_ = false;
    RollingTimeWindow present_interval_window_{256, std::chrono::milliseconds(2000)};
    RollingTimeWindow present_coalesce_window_{256, std::chrono::milliseconds(2000)};

    // Capture-card live wiring (0.8.0). Each is a steady_clock CPU-timing window.
    bool acquire_observed_ = false;
    RollingTimeWindow acquire_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram acquire_hist_;

    // Compositor
    RollingTimeWindow compositor_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram compositor_hist_;
    uint64_t frames_composed_ = 0;
    bool compositor_active_ = false;
    bool vpblt_observed_ = false;
    RollingTimeWindow vpblt_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram vpblt_hist_;

    // GPU-execution-time windows/histograms (real GPU work, distinct from the
    // CPU-submission windows above). Fed from resolved D3D11 timestamp spans.
    RollingTimeWindow composition_gpu_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram composition_gpu_hist_;
    RollingTimeWindow hdr_tonemap_gpu_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram hdr_tonemap_gpu_hist_;
    RollingTimeWindow rgb_to_yuv_gpu_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram rgb_to_yuv_gpu_hist_;
    RollingTimeWindow webcam_upload_gpu_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram webcam_upload_gpu_hist_;

    // Webcam CPU conversion + preview tap copy (CPU-timing windows).
    RollingTimeWindow webcam_convert_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram webcam_convert_hist_;
    RollingTimeWindow preview_copy_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram preview_copy_hist_;
    PreviewTapDiagnostics preview_tap_;

    // Encoder
    uint64_t frames_submitted_ = 0;
    uint64_t forced_keyframes_ = 0;
    uint64_t slot_stalls_ = 0;
    uint64_t frames_duplicated_ = 0;
    uint64_t output_ts_mismatches_ = 0;
    uint64_t keyframe_prediction_mismatches_ = 0;

    // Retained-frame reuse counters
    uint64_t screen_generation_changes_ = 0;
    uint64_t webcam_generation_changes_ = 0;
    uint64_t cursor_only_capture_events_ignored_ = 0;
    uint64_t phase_ring_cursor_only_events_ignored_ = 0;
    uint64_t full_compositions_ = 0;
    uint64_t reused_yuv_frames_ = 0;
    uint64_t yuv_slot_copies_ = 0;
    uint64_t yuv_slot_copies_skipped_ = 0;

    RollingTimeWindow encode_window_{256, std::chrono::milliseconds(2000)};
    RollingTimeWindow submit_window_{256, std::chrono::milliseconds(2000)};
    RollingTimeWindow tick_window_{256, std::chrono::milliseconds(2000)};
    // Whole-session distributions (mergeable, constant memory) — the gate data.
    LatencyHistogram encode_hist_;
    LatencyHistogram submit_hist_;
    LatencyHistogram tick_hist_;

    // Audio
    uint32_t audio_sample_rate_ = 0;
    uint32_t audio_channels_ = 0;
    uint32_t audio_queue_depth_ = 0;
    uint32_t audio_queue_peak_ = 0;
    uint64_t audio_discontinuities_ = 0;
    uint64_t audio_discontinuity_frames_total_ = 0;
    uint32_t audio_discontinuity_frames_longest_ = 0;
    // Per-track degraded/total capture-source counts (ADR 0046). Array size
    // mirrors CodecPrivateData::kMaxAudioTracks; summed in BuildSnapshot.
    std::array<uint32_t, 3> audio_degraded_sources_{};
    std::array<uint32_t, 3> audio_degraded_source_kinds_{};
    std::array<bool, 3> audio_endpoint_in_use_{};
    std::array<uint32_t, 3> audio_total_sources_{};

    // Queues
    uint32_t video_queue_depth_ = 0;
    uint32_t video_queue_peak_ = 0;
    uint32_t audio_premux_depth_ = 0;
    uint32_t audio_premux_peak_ = 0;
    // Queue-saturation event counter: cumulative rising-edge crossings of a
    // monitored queue over its critical threshold. Edge-triggered (a queue that
    // sits saturated counts once, not every publish), tracked per queue so the
    // video mux queue and the bounded audio premux queue are independent.
    uint64_t queue_saturation_events_ = 0;
    bool video_queue_saturated_ = false;
    bool audio_premux_saturated_ = false;

    // Mux / Disk
    uint64_t mux_packets_ = 0;
    uint64_t disk_bytes_written_ = 0;
    RollingTimeWindow write_window_{256, std::chrono::milliseconds(2000)};
    bool mux_process_observed_ = false;
    RollingTimeWindow mux_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram mux_hist_;
    RollingTimeWindow mux_queue_delay_window_{256, std::chrono::milliseconds(2000)};
    LatencyHistogram mux_queue_delay_hist_;
    uint32_t segment_index_ = 0;
    uint32_t segment_count_ = 0;
    uint64_t finalizations_ = 0;
    double last_finalize_ms_ = 0.0;
    uint64_t split_transitions_ = 0;
    uint64_t mux_failures_ = 0;
    uint64_t split_failures_ = 0;
    DiagnosticsSplitTrigger last_split_trigger_ = DiagnosticsSplitTrigger::None;
    bool split_pending_ = false;
    time_point segment_open_time_{};

    // Reorder window (last reported by writer)
    uint32_t reorder_packets_ = 0;
    uint32_t reorder_packets_peak_ = 0;
    uint64_t reorder_bytes_ = 0;
    uint64_t reorder_bytes_peak_ = 0;

    // Rate baselines (set per publish)
    bool have_baseline_ = false;
    time_point last_publish_time_{};
    uint64_t last_frames_emitted_ = 0;
    uint64_t last_frames_encoded_ = 0;
    uint64_t last_disk_bytes_ = 0;

    // Classifier sustained-condition counters
    int sustain_capture_ = 0;
    int sustain_compositor_ = 0;
    int sustain_gpu_ = 0;
    double gpu_exec_p99_ms_ = 0.0; // refreshed by BuildSnapshot before Classify
    int sustain_encoder_ = 0;
    int sustain_audio_ = 0;
    int sustain_muxer_ = 0;
    int sustain_disk_ = 0;
    uint64_t last_dropped_total_ = 0;
    uint64_t last_audio_disc_ = 0;

    // A/V clock slaving: latest per-track raw drift, residual (raw minus applied
    // compensation) and compensation rate (ppm). Array size mirrors
    // CodecPrivateData::kMaxAudioTracks. Protected by mutex_.
    std::array<double, 3> audio_clock_raw_ms_{};
    std::array<double, 3> audio_clock_residual_ms_{};
    std::array<double, 3> audio_clock_ppm_{};
    std::array<bool, 3> audio_clock_valid_{};
    // Latched per track: this track's drift figures are known-invalid, so the
    // snapshot must say so instead of publishing them as measurements.
    std::array<bool, 3> audio_clock_faulted_{};
    std::array<bool, 3> audio_clock_saturated_{};

    // Peak |av_drift_ms| (residual) this session (running maximum). Single source
    // of truth for both the live UI and the session report.
    double peak_av_drift_ms_ = 0.0;
    bool peak_av_drift_valid_ = false;

    // Encoder init parameters (set once by the encoder at configure time).
    EncoderInitInfo encoder_init_;

    // Disk-fill ETA: free bytes on the output drive, polled externally at ~5 Hz.
    uint64_t free_bytes_ = 0;
    bool free_bytes_known_ = false;
};

} // namespace exosnap::engine
