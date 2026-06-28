#pragma once

// Internal per-session aggregator for live pipeline diagnostics.
// Worker threads feed cheap counters/timers; the stats collector calls BuildSnapshot
// (~5 Hz) to produce an immutable RecordingDiagnosticsSnapshot. Time is injected
// (steady_clock::time_point) so aggregation/cadence/expiry are deterministically testable
// without sleeps. Not part of the public API.

#include <recorder_core/pipeline_diagnostics.h>
#include <recorder_core/recorder_session.h>
#include <recorder_core/session_stats.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace recorder_core {

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
    uint64_t encoder_backlog = 2;        // submitted - encoded sustained
    uint32_t mux_queue_warn = 8;         // mux (video) queue depth
    double disk_write_ms_warn = 8.0;     // write-call latency ms
    double audio_queue_warn_ratio = 0.8; // fraction of audio-queue capacity
    double queue_critical_ratio = 0.9;   // fraction of a bounded queue == critical
    double warmup_seconds = 1.0;         // below this, evidence is insufficient (Unknown)
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
    void OnFrameDroppedCfr() noexcept;                                // scheduled tick produced no frame
    void OnFrameDroppedBackpressure() noexcept;                       // encoder input slots all in flight
    void OnObservedFrameInterval(time_point now, double ms) noexcept; // VFR only
    // Present cadence (DXGI OD only): inter-present interval (ms) from LastPresentTime QPC
    // deltas plus the coalesced-update count (AccumulatedFrames). O(1), allocation-free.
    void OnSourcePresentInterval(time_point now, double interval_ms, uint32_t accumulated_frames) noexcept;
    // Compositor (VideoThread) — CPU submission time only
    void OnCompositorSubmit(time_point now, double ms, bool pass_ran) noexcept;
    // Capture-card live wiring (0.8.0): cheap CPU-timing brackets (steady_clock).
    void OnAcquireLatency(time_point now, double ms) noexcept; // acquire+copy (Source Capture)
    void OnVpbltSubmit(time_point now, double ms) noexcept;    // VideoProcessorBlt (Compositor)
    void OnMuxLatency(time_point now, double ms) noexcept;     // mux drain loop (Muxer)
    // Encoder (VideoThread)
    void OnEncodeSubmitted() noexcept;
    void OnEncodeLatency(time_point now, double ms) noexcept;
    void OnForcedKeyframe() noexcept;
    // Audio (AudioThread)
    void SetAudioFormat(uint32_t sample_rate, uint32_t channels) noexcept;
    void OnAudioQueueDepth(uint32_t depth) noexcept;
    void OnAudioDiscontinuity() noexcept;
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
    // PTS inputs for A/V drift (video: VideoThread after encode; audio: AudioThread after encode)
    void OnVideoPts(double ms) noexcept;
    void OnAudioPts(double ms) noexcept;
    // Free-space poll for disk-fill ETA (called from the stats collector at ~5 Hz)
    void UpdateFreeDiskBytes(uint64_t free_bytes) noexcept;

    // ---- collector-thread publish -----------------------------------------
    [[nodiscard]] RecordingDiagnosticsSnapshot BuildSnapshot(time_point now, const SessionStats& stats,
                                                             DiagnosticsLifecycle lifecycle, double elapsed_seconds);

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
    bool interval_observed_ = false;
    RollingTimeWindow interval_window_{256, std::chrono::milliseconds(2000)};

    // Present cadence (DXGI OD only): inter-present interval and coalescing proxy windows.
    bool present_observed_ = false;
    RollingTimeWindow present_interval_window_{256, std::chrono::milliseconds(2000)};
    RollingTimeWindow present_coalesce_window_{256, std::chrono::milliseconds(2000)};

    // Capture-card live wiring (0.8.0). Each is a steady_clock CPU-timing window.
    bool acquire_observed_ = false;
    RollingTimeWindow acquire_window_{256, std::chrono::milliseconds(2000)};

    // Compositor
    RollingTimeWindow compositor_window_{256, std::chrono::milliseconds(2000)};
    uint64_t frames_composed_ = 0;
    bool compositor_active_ = false;
    bool vpblt_observed_ = false;
    RollingTimeWindow vpblt_window_{256, std::chrono::milliseconds(2000)};

    // Encoder
    uint64_t frames_submitted_ = 0;
    uint64_t forced_keyframes_ = 0;
    RollingTimeWindow encode_window_{256, std::chrono::milliseconds(2000)};

    // Audio
    uint32_t audio_sample_rate_ = 0;
    uint32_t audio_channels_ = 0;
    uint32_t audio_queue_depth_ = 0;
    uint32_t audio_queue_peak_ = 0;
    uint64_t audio_discontinuities_ = 0;

    // Queues
    uint32_t video_queue_depth_ = 0;
    uint32_t video_queue_peak_ = 0;
    uint32_t audio_premux_depth_ = 0;
    uint32_t audio_premux_peak_ = 0;

    // Mux / Disk
    uint64_t mux_packets_ = 0;
    uint64_t disk_bytes_written_ = 0;
    RollingTimeWindow write_window_{256, std::chrono::milliseconds(2000)};
    bool mux_process_observed_ = false;
    RollingTimeWindow mux_window_{256, std::chrono::milliseconds(2000)};
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
    int sustain_encoder_ = 0;
    int sustain_audio_ = 0;
    int sustain_muxer_ = 0;
    int sustain_disk_ = 0;
    uint64_t last_dropped_total_ = 0;
    uint64_t last_audio_disc_ = 0;

    // A/V drift: latest PTS (ms) received from each encoder output path.
    // Protected by mutex_. Set to true once the first packet arrives per session.
    double latest_video_pts_ms_ = 0.0;
    double latest_audio_pts_ms_ = 0.0;
    bool have_video_pts_ = false;
    bool have_audio_pts_ = false;

    // Disk-fill ETA: free bytes on the output drive, polled externally at ~5 Hz.
    uint64_t free_bytes_ = 0;
    bool free_bytes_known_ = false;
};

} // namespace recorder_core
