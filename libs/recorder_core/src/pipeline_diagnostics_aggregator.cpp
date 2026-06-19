#include "pipeline_diagnostics_aggregator.h"

#include <cmath>

namespace recorder_core {

namespace {
constexpr uint32_t kAudioPremuxCapacity = 600; // SessionState::kAudioPremuxLimit
} // namespace

DiagnosticsSplitTrigger ToDiagnosticsSplitTrigger(SplitTriggerSource src) noexcept {
    switch (src) {
    case SplitTriggerSource::AutomaticDuration:
        return DiagnosticsSplitTrigger::AutomaticDuration;
    case SplitTriggerSource::AutomaticSize:
        return DiagnosticsSplitTrigger::AutomaticSize;
    case SplitTriggerSource::ManualButton:
        return DiagnosticsSplitTrigger::ManualButton;
    case SplitTriggerSource::Hotkey:
        return DiagnosticsSplitTrigger::Hotkey;
    }
    return DiagnosticsSplitTrigger::None;
}

DiagnosticsStaticConfig MakeDiagnosticsStaticConfig(const RecorderConfig& config) {
    DiagnosticsStaticConfig cfg;

    if (config.target.kind == CaptureTarget::Kind::Window) {
        cfg.source_type = CaptureSourceType::Window;
    } else if (config.crop_region.has_value()) {
        cfg.source_type = CaptureSourceType::Region;
    } else {
        cfg.source_type = CaptureSourceType::Display;
    }

    cfg.split_supported = (config.container == Container::Matroska || config.container == Container::WebM);
    cfg.auto_split = (config.split.duration_ms > 0 || config.split.size_bytes > 0);
    cfg.auto_split_seconds = static_cast<double>(config.split.duration_ms) / 1000.0;

    try {
        const auto root = config.output_path.root_name();
        if (!root.empty()) {
            cfg.output_target = root.string();
        } else {
            cfg.output_target = config.output_path.root_path().string();
        }
    } catch (...) {
        cfg.output_target.clear();
    }

    cfg.audio_present = config.record_audio;
    if (config.record_audio) {
        cfg.audio_track_count =
            config.audio_track_plan.tracks.empty() ? 1u : static_cast<uint32_t>(config.audio_track_plan.tracks.size());
    }

    cfg.video_queue_capacity = 0; // mux queue is unbounded by design (architecture finding)
    cfg.audio_queue_capacity = kAudioPremuxCapacity;
    return cfg;
}

PipelineDiagnosticsAggregator::PipelineDiagnosticsAggregator() = default;

void PipelineDiagnosticsAggregator::Reset(uint64_t generation, const DiagnosticsStaticConfig& cfg) {
    std::lock_guard lk(mutex_);
    generation_ = generation;
    cfg_ = cfg;

    frames_captured_ = 0;
    dropped_coalesced_ = 0;
    dropped_cfr_ = 0;
    dropped_backpressure_ = 0;
    interval_observed_ = false;
    interval_window_.Clear();

    compositor_window_.Clear();
    frames_composed_ = 0;
    compositor_active_ = false;

    frames_submitted_ = 0;
    forced_keyframes_ = 0;
    encode_window_.Clear();

    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_queue_depth_ = 0;
    audio_queue_peak_ = 0;
    audio_discontinuities_ = 0;

    video_queue_depth_ = 0;
    video_queue_peak_ = 0;
    audio_premux_depth_ = 0;
    audio_premux_peak_ = 0;

    mux_packets_ = 0;
    disk_bytes_written_ = 0;
    write_window_.Clear();
    segment_index_ = 0;
    segment_count_ = 0;
    finalizations_ = 0;
    last_finalize_ms_ = 0.0;
    split_transitions_ = 0;
    mux_failures_ = 0;
    split_failures_ = 0;
    last_split_trigger_ = DiagnosticsSplitTrigger::None;
    split_pending_ = false;
    segment_open_time_ = time_point{};

    reorder_packets_ = 0;
    reorder_packets_peak_ = 0;
    reorder_bytes_ = 0;
    reorder_bytes_peak_ = 0;

    have_baseline_ = false;
    last_publish_time_ = time_point{};
    last_frames_emitted_ = 0;
    last_frames_encoded_ = 0;
    last_disk_bytes_ = 0;

    sustain_capture_ = 0;
    sustain_compositor_ = 0;
    sustain_encoder_ = 0;
    sustain_audio_ = 0;
    sustain_muxer_ = 0;
    sustain_disk_ = 0;
    last_dropped_total_ = 0;
    last_audio_disc_ = 0;
}

void PipelineDiagnosticsAggregator::SetThresholds(const DiagnosticsThresholds& thresholds) {
    std::lock_guard lk(mutex_);
    thresholds_ = thresholds;
}

uint64_t PipelineDiagnosticsAggregator::generation() const noexcept {
    std::lock_guard lk(mutex_);
    return generation_;
}

// ---- worker inputs --------------------------------------------------------

void PipelineDiagnosticsAggregator::OnFrameCaptured() noexcept {
    std::lock_guard lk(mutex_);
    ++frames_captured_;
}

void PipelineDiagnosticsAggregator::OnFrameDroppedCoalesced() noexcept {
    std::lock_guard lk(mutex_);
    ++dropped_coalesced_;
}

void PipelineDiagnosticsAggregator::OnFrameDroppedCfr() noexcept {
    std::lock_guard lk(mutex_);
    ++dropped_cfr_;
}

void PipelineDiagnosticsAggregator::OnFrameDroppedBackpressure() noexcept {
    std::lock_guard lk(mutex_);
    ++dropped_backpressure_;
}

void PipelineDiagnosticsAggregator::OnObservedFrameInterval(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    interval_observed_ = true;
    interval_window_.Add(now, ms);
}

void PipelineDiagnosticsAggregator::OnCompositorSubmit(time_point now, double ms, bool pass_ran) noexcept {
    std::lock_guard lk(mutex_);
    compositor_active_ = pass_ran;
    if (pass_ran) {
        ++frames_composed_;
        compositor_window_.Add(now, ms);
    }
}

void PipelineDiagnosticsAggregator::OnEncodeSubmitted() noexcept {
    std::lock_guard lk(mutex_);
    ++frames_submitted_;
}

void PipelineDiagnosticsAggregator::OnEncodeLatency(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    encode_window_.Add(now, ms);
}

void PipelineDiagnosticsAggregator::OnForcedKeyframe() noexcept {
    std::lock_guard lk(mutex_);
    ++forced_keyframes_;
}

void PipelineDiagnosticsAggregator::SetAudioFormat(uint32_t sample_rate, uint32_t channels) noexcept {
    std::lock_guard lk(mutex_);
    audio_sample_rate_ = sample_rate;
    audio_channels_ = channels;
}

void PipelineDiagnosticsAggregator::OnAudioQueueDepth(uint32_t depth) noexcept {
    std::lock_guard lk(mutex_);
    audio_queue_depth_ = depth;
    if (depth > audio_queue_peak_) {
        audio_queue_peak_ = depth;
    }
}

void PipelineDiagnosticsAggregator::OnAudioDiscontinuity() noexcept {
    std::lock_guard lk(mutex_);
    ++audio_discontinuities_;
}

void PipelineDiagnosticsAggregator::OnVideoQueueDepth(uint32_t depth) noexcept {
    std::lock_guard lk(mutex_);
    video_queue_depth_ = depth;
    if (depth > video_queue_peak_) {
        video_queue_peak_ = depth;
    }
}

void PipelineDiagnosticsAggregator::OnAudioPremuxDepth(uint32_t depth) noexcept {
    std::lock_guard lk(mutex_);
    audio_premux_depth_ = depth;
    if (depth > audio_premux_peak_) {
        audio_premux_peak_ = depth;
    }
}

void PipelineDiagnosticsAggregator::OnMuxPacket(uint64_t bytes) noexcept {
    std::lock_guard lk(mutex_);
    ++mux_packets_;
    (void)bytes; // encoded byte total is tracked via SessionStats; mux byte boundary is OnDiskWrite
}

void PipelineDiagnosticsAggregator::OnDiskWrite(time_point now, double ms, uint64_t bytes) noexcept {
    std::lock_guard lk(mutex_);
    disk_bytes_written_ += bytes;
    write_window_.Add(now, ms);
}

void PipelineDiagnosticsAggregator::OnSegmentOpened(uint32_t index) noexcept {
    std::lock_guard lk(mutex_);
    segment_index_ = index;
    segment_open_time_ = std::chrono::steady_clock::now();
    if (index + 1 > segment_count_) {
        segment_count_ = index + 1;
    }
}

void PipelineDiagnosticsAggregator::OnSegmentFinalized(double ms, bool succeeded) noexcept {
    std::lock_guard lk(mutex_);
    ++finalizations_;
    last_finalize_ms_ = ms;
    if (!succeeded) {
        ++split_failures_;
    }
}

void PipelineDiagnosticsAggregator::OnSplitTransition(DiagnosticsSplitTrigger trigger) noexcept {
    std::lock_guard lk(mutex_);
    ++split_transitions_;
    last_split_trigger_ = trigger;
    split_pending_ = false;
}

void PipelineDiagnosticsAggregator::OnMuxFailure() noexcept {
    std::lock_guard lk(mutex_);
    ++mux_failures_;
}

void PipelineDiagnosticsAggregator::OnReorderWindow(uint32_t packets, uint32_t peak_packets, uint64_t bytes,
                                                    uint64_t peak_bytes) noexcept {
    std::lock_guard lk(mutex_);
    reorder_packets_ = packets;
    reorder_bytes_ = bytes;
    if (peak_packets > reorder_packets_peak_) {
        reorder_packets_peak_ = peak_packets;
    }
    if (peak_bytes > reorder_bytes_peak_) {
        reorder_bytes_peak_ = peak_bytes;
    }
}

void PipelineDiagnosticsAggregator::SetSplitPending(bool pending) noexcept {
    std::lock_guard lk(mutex_);
    split_pending_ = pending;
}

// ---- publish --------------------------------------------------------------

RecordingDiagnosticsSnapshot PipelineDiagnosticsAggregator::BuildSnapshot(time_point now, const SessionStats& stats,
                                                                          DiagnosticsLifecycle lifecycle,
                                                                          double elapsed_seconds) {
    std::lock_guard lk(mutex_);

    RecordingDiagnosticsSnapshot s;
    s.session_generation = generation_;
    s.lifecycle = lifecycle;
    s.elapsed_seconds = elapsed_seconds;
    s.valid = (lifecycle != DiagnosticsLifecycle::Idle && lifecycle != DiagnosticsLifecycle::Initializing);

    const bool recording = (lifecycle == DiagnosticsLifecycle::Recording);

    double dt = 0.0;
    if (have_baseline_) {
        dt = std::chrono::duration<double>(now - last_publish_time_).count();
    }
    const bool can_rate = have_baseline_ && dt > 1e-6;

    // ---- Capture ----
    CaptureDiagnostics& cap = s.capture;
    cap.target_fps = (stats.frame_rate_den > 0)
                         ? static_cast<double>(stats.frame_rate_num) / static_cast<double>(stats.frame_rate_den)
                         : 0.0;
    cap.frames_captured = frames_captured_;
    cap.frames_emitted = stats.video_frames_captured; // stats field is misnamed; it counts emitted frames
    cap.frames_dropped_coalesced = dropped_coalesced_;
    cap.frames_dropped_cfr = dropped_cfr_;
    cap.frames_dropped_backpressure = dropped_backpressure_;
    cap.frames_duplicated = stats.duplicated_video_frames;
    cap.source_type = cfg_.source_type;
    cap.source_loss = stats.source_loss;
    if (can_rate && stats.video_frames_captured >= last_frames_emitted_) {
        cap.actual_fps = static_cast<double>(stats.video_frames_captured - last_frames_emitted_) / dt;
    }
    if (interval_observed_) {
        const RollingTimeWindow::Aggregate a = interval_window_.Compute(now);
        cap.frame_interval_ms = a.average;
        cap.interval_observed = MetricAvailability::Available;
    } else {
        cap.frame_interval_ms = (cap.target_fps > 0.0) ? 1000.0 / cap.target_fps : 0.0;
        cap.interval_observed = MetricAvailability::Unavailable;
    }

    // ---- Compositor (CPU submission timing only) ----
    const RollingTimeWindow::Aggregate comp = compositor_window_.Compute(now);
    s.compositor.active = compositor_active_;
    s.compositor.latest_ms = comp.latest;
    s.compositor.average_ms = comp.average;
    s.compositor.peak_ms = comp.peak;
    s.compositor.frames_composed = frames_composed_;

    // ---- Encoder ----
    EncoderDiagnostics& enc = s.video_encoder;
    const RollingTimeWindow::Aggregate el = encode_window_.Compute(now);
    enc.latest_ms = el.latest;
    enc.average_ms = el.average;
    enc.peak_ms = el.peak;
    enc.frames_submitted = frames_submitted_;
    enc.frames_encoded = stats.encoded_video_packets;
    enc.backlog =
        (frames_submitted_ > stats.encoded_video_packets) ? (frames_submitted_ - stats.encoded_video_packets) : 0;
    enc.forced_keyframes = forced_keyframes_;
    enc.codec = stats.video_codec;
    enc.width = stats.output_size.width;
    enc.height = stats.output_size.height;
    enc.cfr = stats.cfr;
    if (can_rate && stats.encoded_video_packets >= last_frames_encoded_) {
        enc.output_fps = static_cast<double>(stats.encoded_video_packets - last_frames_encoded_) / dt;
    }

    // ---- Audio ----
    AudioDiagnostics& au = s.audio;
    au.active = cfg_.audio_present;
    au.packets_encoded = stats.audio_packets;
    au.bytes_encoded = stats.audio_bytes;
    au.queue_depth = audio_queue_depth_;
    au.queue_peak = audio_queue_peak_;
    au.discontinuities = audio_discontinuities_;
    au.discontinuity_availability = MetricAvailability::Available;
    au.sample_rate = audio_sample_rate_;
    au.channels = audio_channels_;
    au.codec = stats.audio_codec;
    au.track_count = cfg_.audio_track_count;

    // ---- Queues ----
    s.video_queue.current_depth = video_queue_depth_;
    s.video_queue.peak_depth = video_queue_peak_;
    s.video_queue.capacity = cfg_.video_queue_capacity;
    s.video_queue.bounded = (cfg_.video_queue_capacity > 0);
    s.audio_queue.current_depth = audio_premux_depth_;
    s.audio_queue.peak_depth = audio_premux_peak_;
    s.audio_queue.capacity = cfg_.audio_queue_capacity;
    s.audio_queue.bounded = (cfg_.audio_queue_capacity > 0);

    // ---- Mux / Disk (single filesystem write boundary) ----
    const RollingTimeWindow::Aggregate wl = write_window_.Compute(now);
    double throughput = 0.0;
    if (can_rate && disk_bytes_written_ >= last_disk_bytes_) {
        throughput = (static_cast<double>(disk_bytes_written_ - last_disk_bytes_) / (1024.0 * 1024.0)) / dt;
    }

    MuxDiagnostics& mux = s.mux;
    mux.packets_processed = mux_packets_;
    mux.bytes_written = disk_bytes_written_;
    mux.throughput_mib_s = throughput;
    mux.latest_write_ms = wl.latest;
    mux.average_write_ms = wl.average;
    mux.peak_write_ms = wl.peak;
    mux.current_segment_index = segment_index_;
    mux.segment_count = segment_count_;
    mux.finalizations = finalizations_;
    mux.latest_finalize_ms = last_finalize_ms_;
    mux.split_transitions = split_transitions_;
    mux.failures = mux_failures_;
    mux.reorder_packets = reorder_packets_;
    mux.reorder_packets_peak = reorder_packets_peak_;
    mux.reorder_bytes = reorder_bytes_;
    mux.reorder_bytes_peak = reorder_bytes_peak_;
    // The reorder window / segment concept only exists for the streaming Matroska writer.
    mux.availability = cfg_.split_supported ? MetricAvailability::Available : MetricAvailability::Unavailable;

    DiskDiagnostics& disk = s.disk;
    disk.bytes_written = disk_bytes_written_;
    disk.throughput_mib_s = throughput;
    disk.latest_write_ms = wl.latest;
    disk.average_write_ms = wl.average;
    disk.peak_write_ms = wl.peak;
    disk.output_target = cfg_.output_target;
    disk.write_failures = mux_failures_;
    // Only the streaming Matroska writer exposes a measurable filesystem write boundary;
    // the MP4 path (IMFSinkWriter) buffers internally with no measurable write call.
    disk.latency_availability = cfg_.split_supported ? MetricAvailability::Available : MetricAvailability::Unavailable;

    // ---- Split ----
    SplitDiagnostics& sp = s.split;
    sp.split_supported = cfg_.split_supported;
    sp.current_segment = (segment_count_ > 0) ? segment_index_ + 1 : 0;
    sp.completed_segments = static_cast<uint32_t>(split_transitions_);
    sp.split_pending = split_pending_;
    sp.last_trigger = last_split_trigger_;
    sp.last_finalize_ms = last_finalize_ms_;
    sp.split_failures = split_failures_;
    sp.availability = cfg_.split_supported ? MetricAvailability::Available : MetricAvailability::Unavailable;
    if (cfg_.split_supported && cfg_.auto_split && recording && cfg_.auto_split_seconds > 0.0) {
        const double since = segment_open_time_ != time_point{}
                                 ? std::chrono::duration<double>(now - segment_open_time_).count()
                                 : elapsed_seconds;
        sp.seconds_until_auto_split = std::max(0.0, cfg_.auto_split_seconds - since);
    } else {
        sp.seconds_until_auto_split = -1.0;
    }

    // ---- Classification ----
    std::string reason;
    PipelineHealth health = PipelineHealth::Idle;
    s.bottleneck = Classify(s, recording, reason, health);
    s.bottleneck_reason = reason;
    s.health = health;

    // ---- Update rate baselines ----
    have_baseline_ = true;
    last_publish_time_ = now;
    last_frames_emitted_ = stats.video_frames_captured;
    last_frames_encoded_ = stats.encoded_video_packets;
    last_disk_bytes_ = disk_bytes_written_;

    return s;
}

PipelineBottleneck PipelineDiagnosticsAggregator::Classify(const RecordingDiagnosticsSnapshot& s, bool recording,
                                                           std::string& reason, PipelineHealth& health) {
    // Problematic drops exclude benign capture coalescing (source faster than target).
    const uint64_t problem_drops = s.capture.frames_dropped_cfr + s.capture.frames_dropped_backpressure;

    if (!recording) {
        sustain_capture_ = sustain_compositor_ = sustain_encoder_ = 0;
        sustain_audio_ = sustain_muxer_ = sustain_disk_ = 0;
        last_dropped_total_ = problem_drops;
        last_audio_disc_ = s.audio.discontinuities;
        reason.clear();
        if (s.lifecycle == DiagnosticsLifecycle::Idle || s.lifecycle == DiagnosticsLifecycle::Initializing) {
            health = PipelineHealth::Idle;
        } else if (s.lifecycle == DiagnosticsLifecycle::Failed || s.mux.failures > 0 || s.split.split_failures > 0) {
            health = PipelineHealth::Critical;
        } else {
            health = PipelineHealth::Good; // Paused / Stopping / Completed
        }
        return PipelineBottleneck::None;
    }

    const double budget_ms = (s.capture.target_fps > 0.0) ? 1000.0 / s.capture.target_fps : (1000.0 / 60.0);

    const bool drops_rising = problem_drops > last_dropped_total_;
    const bool audio_disc_rising = s.audio.discontinuities > last_audio_disc_;

    const bool downstream_saturated = s.video_queue.current_depth >= thresholds_.mux_queue_warn;
    const bool cap_cond = s.capture.target_fps > 0.0 && s.capture.actual_fps > 0.0 &&
                          s.capture.actual_fps < s.capture.target_fps * thresholds_.capture_fps_ratio &&
                          !downstream_saturated;
    const bool comp_cond =
        s.compositor.active && s.compositor.average_ms > budget_ms * thresholds_.compositor_budget_ratio;
    const bool enc_cond = s.video_encoder.backlog >= thresholds_.encoder_backlog ||
                          s.video_encoder.average_ms > budget_ms * thresholds_.encoder_budget_ratio;
    const bool disk_cond = s.disk.average_write_ms > thresholds_.disk_write_ms_warn;
    const bool mux_cond = s.video_queue.current_depth >= thresholds_.mux_queue_warn && !disk_cond;
    const bool audio_cond = audio_disc_rising || (s.audio_queue.bounded && s.audio_queue.capacity > 0 &&
                                                  static_cast<double>(s.audio_queue.current_depth) >=
                                                      s.audio_queue.capacity * thresholds_.audio_queue_warn_ratio);

    auto bump = [](int& c, bool cond) { c = cond ? c + 1 : 0; };
    bump(sustain_capture_, cap_cond);
    bump(sustain_compositor_, comp_cond);
    bump(sustain_encoder_, enc_cond);
    bump(sustain_disk_, disk_cond);
    bump(sustain_muxer_, mux_cond);
    bump(sustain_audio_, audio_cond);

    last_dropped_total_ = problem_drops;
    last_audio_disc_ = s.audio.discontinuities;

    const int n = thresholds_.sustain_samples;
    const bool warming = s.elapsed_seconds < thresholds_.warmup_seconds || s.video_encoder.frames_encoded == 0;

    PipelineBottleneck bottleneck = PipelineBottleneck::None;
    // Priority: most-downstream sustained constraint wins (it is the true root).
    if (sustain_disk_ >= n) {
        bottleneck = PipelineBottleneck::Disk;
        reason = "Write latency high";
    } else if (sustain_muxer_ >= n) {
        bottleneck = PipelineBottleneck::Muxer;
        reason = "Mux queue backing up";
    } else if (sustain_encoder_ >= n) {
        bottleneck = PipelineBottleneck::VideoEncoder;
        reason = "Encoder backlog rising";
    } else if (sustain_compositor_ >= n) {
        bottleneck = PipelineBottleneck::Compositor;
        reason = "Composition near frame budget";
    } else if (sustain_capture_ >= n) {
        bottleneck = PipelineBottleneck::Capture;
        reason = "Capture below target FPS";
    } else if (sustain_audio_ >= n) {
        bottleneck = PipelineBottleneck::Audio;
        reason = "Audio drops / queue pressure";
    } else if (warming) {
        bottleneck = PipelineBottleneck::Unknown;
        reason = "Gathering data";
    } else {
        bottleneck = PipelineBottleneck::None;
        reason.clear();
    }

    // Health from measurable conditions.
    const bool queue_critical =
        s.audio_queue.bounded && s.audio_queue.capacity > 0 &&
        static_cast<double>(s.audio_queue.current_depth) >= s.audio_queue.capacity * thresholds_.queue_critical_ratio;
    if (s.mux.failures > 0 || s.disk.write_failures > 0 || s.split.split_failures > 0 || queue_critical) {
        health = PipelineHealth::Critical;
    } else if ((bottleneck != PipelineBottleneck::None && bottleneck != PipelineBottleneck::Unknown) ||
               (drops_rising && problem_drops > 0)) {
        health = PipelineHealth::Warning;
    } else {
        health = PipelineHealth::Good;
    }

    return bottleneck;
}

} // namespace recorder_core
