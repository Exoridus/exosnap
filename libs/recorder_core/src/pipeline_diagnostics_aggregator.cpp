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

    present_observed_ = false;
    present_interval_window_.Clear();
    present_coalesce_window_.Clear();

    acquire_observed_ = false;
    acquire_window_.Clear();
    acquire_hist_.Clear();

    compositor_window_.Clear();
    compositor_hist_.Clear();
    frames_composed_ = 0;
    compositor_active_ = false;
    vpblt_observed_ = false;
    vpblt_window_.Clear();
    vpblt_hist_.Clear();

    composition_gpu_window_.Clear();
    composition_gpu_hist_.Clear();
    hdr_tonemap_gpu_window_.Clear();
    hdr_tonemap_gpu_hist_.Clear();
    rgb_to_yuv_gpu_window_.Clear();
    rgb_to_yuv_gpu_hist_.Clear();
    webcam_upload_gpu_window_.Clear();
    webcam_upload_gpu_hist_.Clear();
    webcam_convert_window_.Clear();
    webcam_convert_hist_.Clear();
    preview_copy_window_.Clear();
    preview_copy_hist_.Clear();

    frames_submitted_ = 0;
    forced_keyframes_ = 0;
    slot_stalls_ = 0;
    frames_duplicated_ = 0;

    // Retained-frame reuse counters
    screen_generation_changes_ = 0;
    webcam_generation_changes_ = 0;
    cursor_only_capture_events_ignored_ = 0;
    phase_ring_cursor_only_events_ignored_ = 0;
    full_compositions_ = 0;
    reused_yuv_frames_ = 0;
    yuv_slot_copies_ = 0;
    yuv_slot_copies_skipped_ = 0;

    encode_window_.Clear();
    submit_window_.Clear();
    tick_window_.Clear();
    encode_hist_.Clear();
    submit_hist_.Clear();
    tick_hist_.Clear();

    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    audio_queue_depth_ = 0;
    audio_queue_peak_ = 0;
    audio_discontinuities_ = 0;
    audio_degraded_sources_.fill(0);
    audio_total_sources_.fill(0);

    video_queue_depth_ = 0;
    video_queue_peak_ = 0;
    audio_premux_depth_ = 0;
    audio_premux_peak_ = 0;
    queue_saturation_events_ = 0;
    video_queue_saturated_ = false;
    audio_premux_saturated_ = false;

    mux_packets_ = 0;
    disk_bytes_written_ = 0;
    write_window_.Clear();
    mux_process_observed_ = false;
    mux_window_.Clear();
    mux_hist_.Clear();
    mux_queue_delay_window_.Clear();
    mux_queue_delay_hist_.Clear();
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

    audio_clock_raw_ms_.fill(0.0);
    audio_clock_residual_ms_.fill(0.0);
    audio_clock_ppm_.fill(0.0);
    audio_clock_valid_.fill(false);
    peak_av_drift_ms_ = 0.0;
    peak_av_drift_valid_ = false;
    encoder_init_ = EncoderInitInfo{};

    free_bytes_ = 0;
    free_bytes_known_ = false;
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

void PipelineDiagnosticsAggregator::OnSourcePresentInterval(time_point now, double interval_ms,
                                                            uint32_t accumulated_frames) noexcept {
    std::lock_guard lk(mutex_);
    present_observed_ = true;
    present_interval_window_.Add(now, interval_ms);
    // AccumulatedFrames can be 0 for cursor-only updates; clamp to >=1 so the coalescing
    // average reflects "presents per acquire" without being dragged toward zero.
    const double frames = accumulated_frames > 0 ? static_cast<double>(accumulated_frames) : 1.0;
    present_coalesce_window_.Add(now, frames);
}

void PipelineDiagnosticsAggregator::OnCompositorSubmit(time_point now, double ms, bool pass_ran) noexcept {
    std::lock_guard lk(mutex_);
    compositor_active_ = pass_ran;
    if (pass_ran) {
        ++frames_composed_;
        compositor_window_.Add(now, ms);
        compositor_hist_.Add(ms);
    }
}

void PipelineDiagnosticsAggregator::OnAcquireLatency(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    acquire_observed_ = true;
    acquire_window_.Add(now, ms);
    acquire_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnVpbltSubmit(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    vpblt_observed_ = true;
    vpblt_window_.Add(now, ms);
    vpblt_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnMuxLatency(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    mux_process_observed_ = true;
    mux_window_.Add(now, ms);
    mux_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnCompositionGpuTime(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    composition_gpu_window_.Add(now, ms);
    composition_gpu_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnHdrTonemapGpuTime(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    hdr_tonemap_gpu_window_.Add(now, ms);
    hdr_tonemap_gpu_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnRgbToYuvGpuTime(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    rgb_to_yuv_gpu_window_.Add(now, ms);
    rgb_to_yuv_gpu_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnWebcamUploadGpuTime(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    webcam_upload_gpu_window_.Add(now, ms);
    webcam_upload_gpu_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnWebcamConvert(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    webcam_convert_window_.Add(now, ms);
    webcam_convert_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnPreviewCopy(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    preview_copy_window_.Add(now, ms);
    preview_copy_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnMuxQueueDelay(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    mux_queue_delay_window_.Add(now, ms);
    mux_queue_delay_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnFrameDuplicated() noexcept {
    std::lock_guard lk(mutex_);
    ++frames_duplicated_;
}

void PipelineDiagnosticsAggregator::OnScreenGenerationChanged() noexcept {
    std::lock_guard lk(mutex_);
    ++screen_generation_changes_;
}

void PipelineDiagnosticsAggregator::OnWebcamGenerationChanged() noexcept {
    std::lock_guard lk(mutex_);
    ++webcam_generation_changes_;
}

void PipelineDiagnosticsAggregator::OnCursorOnlyCaptureEventIgnored() noexcept {
    std::lock_guard lk(mutex_);
    ++cursor_only_capture_events_ignored_;
}

void PipelineDiagnosticsAggregator::OnPhaseRingCursorOnlyEventIgnored() noexcept {
    std::lock_guard lk(mutex_);
    ++phase_ring_cursor_only_events_ignored_;
}

void PipelineDiagnosticsAggregator::OnFullComposition() noexcept {
    std::lock_guard lk(mutex_);
    ++full_compositions_;
}

void PipelineDiagnosticsAggregator::OnReusedYuvFrame() noexcept {
    std::lock_guard lk(mutex_);
    ++reused_yuv_frames_;
}

void PipelineDiagnosticsAggregator::OnYuvSlotCopy() noexcept {
    std::lock_guard lk(mutex_);
    ++yuv_slot_copies_;
}

void PipelineDiagnosticsAggregator::OnYuvSlotCopySkipped() noexcept {
    std::lock_guard lk(mutex_);
    ++yuv_slot_copies_skipped_;
}

void PipelineDiagnosticsAggregator::OnEncodeSubmitted() noexcept {
    std::lock_guard lk(mutex_);
    ++frames_submitted_;
}

void PipelineDiagnosticsAggregator::OnEncodeLatency(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    encode_window_.Add(now, ms);
    encode_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnEncodeSubmitCost(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    submit_window_.Add(now, ms);
    submit_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnVideoTickTime(time_point now, double ms) noexcept {
    std::lock_guard lk(mutex_);
    tick_window_.Add(now, ms);
    tick_hist_.Add(ms);
}

void PipelineDiagnosticsAggregator::OnSlotStall() noexcept {
    std::lock_guard lk(mutex_);
    ++slot_stalls_;
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

void PipelineDiagnosticsAggregator::OnAudioSourceHealth(uint32_t track_id, uint32_t degraded_sources,
                                                        uint32_t total_sources) noexcept {
    std::lock_guard lk(mutex_);
    if (track_id < audio_degraded_sources_.size()) {
        audio_degraded_sources_[track_id] = degraded_sources;
        audio_total_sources_[track_id] = total_sources;
    }
}

void PipelineDiagnosticsAggregator::OnVideoQueueDepth(uint32_t depth) noexcept {
    std::lock_guard lk(mutex_);
    video_queue_depth_ = depth;
    if (depth > video_queue_peak_) {
        video_queue_peak_ = depth;
    }
    // The post-encode mux queue is unbounded by design, so there is no ratio to
    // cross; its saturation is the same absolute depth the classifier warns on
    // (mux_queue_warn). Count only the rising edge so a queue that stays backed
    // up is one event, not one per publish.
    const bool saturated = depth >= thresholds_.mux_queue_warn;
    if (saturated && !video_queue_saturated_) {
        ++queue_saturation_events_;
    }
    video_queue_saturated_ = saturated;
}

void PipelineDiagnosticsAggregator::OnAudioPremuxDepth(uint32_t depth) noexcept {
    std::lock_guard lk(mutex_);
    audio_premux_depth_ = depth;
    if (depth > audio_premux_peak_) {
        audio_premux_peak_ = depth;
    }
    // The premux queue is bounded, so saturation is a fraction of its capacity
    // (queue_critical_ratio). Count only the rising edge.
    bool saturated = false;
    if (cfg_.audio_queue_capacity > 0) {
        saturated = static_cast<double>(depth) >=
                    static_cast<double>(cfg_.audio_queue_capacity) * thresholds_.queue_critical_ratio;
    }
    if (saturated && !audio_premux_saturated_) {
        ++queue_saturation_events_;
    }
    audio_premux_saturated_ = saturated;
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

void PipelineDiagnosticsAggregator::OnAudioClockSlaving(uint32_t track_id, double raw_drift_ms, double residual_ms,
                                                        double applied_ppm) noexcept {
    std::lock_guard lk(mutex_);
    if (track_id >= audio_clock_valid_.size()) {
        return;
    }
    audio_clock_raw_ms_[track_id] = raw_drift_ms;
    audio_clock_residual_ms_[track_id] = residual_ms;
    audio_clock_ppm_[track_id] = applied_ppm;
    audio_clock_valid_[track_id] = true;
}

void PipelineDiagnosticsAggregator::SetEncoderInitInfo(const EncoderInitInfo& info) noexcept {
    std::lock_guard lk(mutex_);
    encoder_init_ = info;
}

void PipelineDiagnosticsAggregator::UpdateFreeDiskBytes(uint64_t free_bytes) noexcept {
    std::lock_guard lk(mutex_);
    free_bytes_ = free_bytes;
    free_bytes_known_ = true;
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

    if (acquire_observed_) {
        const RollingTimeWindow::Aggregate a = acquire_window_.Compute(now);
        cap.acquire_latest_ms = a.latest;
        cap.acquire_average_ms = a.average;
        cap.acquire_peak_ms = a.peak;
        cap.acquire_availability = (a.count > 0) ? MetricAvailability::Available : MetricAvailability::Unavailable;
    }

    // ---- Present cadence (VRR/CFR judder correlation) ----
    // DXGI OD only (present_observed_ stays false for WGC). Gated by warm-up and a minimum
    // sample count so brief scheduler/QPC jitter at session start is not mis-read as judder.
    // The 2 s rolling window naturally expires stale spikes. Default stays Unavailable.
    constexpr std::size_t kMinPresentSamples = 8;
    if (present_observed_ && elapsed_seconds >= thresholds_.warmup_seconds) {
        const RollingTimeWindow::Aggregate pres = present_interval_window_.Compute(now);
        const RollingTimeWindow::Aggregate coal = present_coalesce_window_.Compute(now);
        if (pres.count >= kMinPresentSamples) {
            cap.source_present_interval_ms = pres.average;
            cap.source_present_jitter_ms = (pres.peak > pres.average) ? (pres.peak - pres.average) : 0.0;
            cap.source_coalesce_ratio = (coal.count > 0) ? coal.average : 1.0;
            cap.present_cadence_availability = MetricAvailability::Available;
        }
    }

    // ---- Compositor (CPU submission timing only) ----
    const RollingTimeWindow::Aggregate comp = compositor_window_.Compute(now);
    s.compositor.active = compositor_active_;
    s.compositor.overlay_omitted = stats.webcam_overlay_omitted;
    s.compositor.latest_ms = comp.latest;
    s.compositor.average_ms = comp.average;
    s.compositor.peak_ms = comp.peak;
    s.compositor.frames_composed = frames_composed_;

    if (vpblt_observed_) {
        const RollingTimeWindow::Aggregate vp = vpblt_window_.Compute(now);
        s.compositor.vpblt_latest_ms = vp.latest;
        s.compositor.vpblt_average_ms = vp.average;
        s.compositor.vpblt_peak_ms = vp.peak;
        s.compositor.vpblt_availability =
            (vp.count > 0) ? MetricAvailability::Available : MetricAvailability::Unavailable;
    }

    // ---- Encoder ----
    EncoderDiagnostics& enc = s.video_encoder;
    const RollingTimeWindow::Aggregate el = encode_window_.Compute(now);
    enc.latest_ms = el.latest;
    enc.average_ms = el.average;
    enc.peak_ms = el.peak;
    enc.p50_ms = encode_window_.Percentile(now, 0.50);
    enc.p99_ms = encode_window_.Percentile(now, 0.99);
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

    // ---- Video-thread frame time (whole-tick) ----
    VideoTimingDiagnostics& vt = s.video_timing;
    const RollingTimeWindow::Aggregate tk = tick_window_.Compute(now);
    vt.budget_ms = (cap.target_fps > 0.0) ? 1000.0 / cap.target_fps : 0.0;
    if (tk.count > 0) {
        vt.tick_p50_ms = tick_window_.Percentile(now, 0.50);
        vt.tick_p99_ms = tick_window_.Percentile(now, 0.99);
        vt.tick_peak_ms = tk.peak;
        vt.availability = MetricAvailability::Available;
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
    uint32_t degraded_total = 0;
    for (const uint32_t d : audio_degraded_sources_) {
        degraded_total += d;
    }
    au.degraded_sources = degraded_total;
    au.source_degraded = degraded_total > 0;

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

    if (mux_process_observed_) {
        const RollingTimeWindow::Aggregate mp = mux_window_.Compute(now);
        mux.process_latest_ms = mp.latest;
        mux.process_average_ms = mp.average;
        mux.process_peak_ms = mp.peak;
        mux.process_availability = (mp.count > 0) ? MetricAvailability::Available : MetricAvailability::Unavailable;
    }

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

    // ---- A/V drift + clock slaving ----
    // av_drift_ms surfaces the RESIDUAL — the misalignment that actually lands in
    // the file after clock slaving pulled the audio timeline toward the QPC axis.
    // Each audio worker reports raw drift (device-vs-QPC), residual, and its
    // current compensation ppm. With several device-backed tracks the
    // largest-|residual| track is surfaced (and its raw drift + ppm alongside);
    // multi-source merges never report, so the metric stays Unavailable rather
    // than guessing. clock_slaving_active latches from any engaged track.
    s.av_drift_ms = 0.0;
    s.av_drift_raw_ms = 0.0;
    s.clock_slaving_ppm = 0.0;
    s.clock_slaving_active = false;
    s.av_drift_availability = MetricAvailability::Unavailable;
    for (std::size_t i = 0; i < audio_clock_valid_.size(); ++i) {
        if (!audio_clock_valid_[i]) {
            continue;
        }
        if (s.av_drift_availability == MetricAvailability::Unavailable ||
            std::abs(audio_clock_residual_ms_[i]) > std::abs(s.av_drift_ms)) {
            s.av_drift_ms = audio_clock_residual_ms_[i];
            s.av_drift_raw_ms = audio_clock_raw_ms_[i];
            s.clock_slaving_ppm = audio_clock_ppm_[i];
        }
        s.av_drift_availability = MetricAvailability::Available;
        // A track is actively slaved if it commands a rate or has already shifted
        // the timeline (raw != residual, i.e. some compensation was applied).
        if (audio_clock_ppm_[i] != 0.0 || std::abs(audio_clock_raw_ms_[i] - audio_clock_residual_ms_[i]) > 1e-9) {
            s.clock_slaving_active = true;
        }
    }

    // Running peak of the residual magnitude, so the live UI and the session
    // report share one authoritative value instead of each accumulating
    // independently.
    if (s.av_drift_availability == MetricAvailability::Available) {
        const double magnitude = std::abs(s.av_drift_ms);
        if (!peak_av_drift_valid_ || magnitude > peak_av_drift_ms_) {
            peak_av_drift_ms_ = magnitude;
        }
        peak_av_drift_valid_ = true;
    }
    s.peak_av_drift_ms = peak_av_drift_ms_;
    s.peak_av_drift_availability =
        peak_av_drift_valid_ ? MetricAvailability::Available : MetricAvailability::Unavailable;

    // Encoder init parameters, captured once at configure time.
    s.encoder_init = encoder_init_;

    // ---- Duration skew ----
    // Total media-duration mismatch between the video and audio timelines, from the
    // live SessionStats durations. A large, sustained skew means one timeline is being
    // compressed relative to the other (e.g. a starving encoder) — the honest
    // counterpart to the CFR scheduler's wall-clock resync. Distinct from av_drift_ms.
    if (stats.video_duration_ns > 0 && stats.audio_duration_ns > 0) {
        const double vd = static_cast<double>(stats.video_duration_ns) / 1e6;
        const double ad = static_cast<double>(stats.audio_duration_ns) / 1e6;
        s.duration_skew_ms = (vd > ad) ? (vd - ad) : (ad - vd);
        s.duration_skew_availability = MetricAvailability::Available;
    } else {
        s.duration_skew_ms = 0.0;
        s.duration_skew_availability = MetricAvailability::Unavailable;
    }

    // ---- Disk-fill ETA ----
    // throughput is the interval MiB/s already computed above.
    // free_bytes_ is polled externally via UpdateFreeDiskBytes at ~5 Hz.
    if (throughput > 0.001 && free_bytes_known_ && free_bytes_ > 0) {
        const double free_mib = static_cast<double>(free_bytes_) / (1024.0 * 1024.0);
        s.disk_fill_eta_seconds = free_mib / throughput;
    } else {
        s.disk_fill_eta_seconds = -1.0;
    }

    // ---- Retained-frame reuse counters (spec section 13) ----
    s.screen_generation_changes = screen_generation_changes_;
    s.webcam_generation_changes = webcam_generation_changes_;
    s.cursor_only_capture_events_ignored = cursor_only_capture_events_ignored_;
    s.phase_ring_cursor_only_events_ignored = phase_ring_cursor_only_events_ignored_;
    s.full_compositions = full_compositions_;
    s.reused_yuv_frames = reused_yuv_frames_;
    s.yuv_slot_copies = yuv_slot_copies_;
    s.yuv_slot_copies_skipped = yuv_slot_copies_skipped_;

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

PerfWindowSample PipelineDiagnosticsAggregator::SamplePerfWindow(time_point now) const {
    std::lock_guard lk(mutex_);
    PerfWindowSample p;
    p.acquire = SampleStage(acquire_window_, now);
    p.composition_cpu = SampleStage(compositor_window_, now);
    p.composition_gpu = SampleStage(composition_gpu_window_, now);
    p.hdr_tonemap_gpu = SampleStage(hdr_tonemap_gpu_window_, now);
    p.rgb_to_yuv_cpu = SampleStage(vpblt_window_, now);
    p.rgb_to_yuv_gpu = SampleStage(rgb_to_yuv_gpu_window_, now);
    p.encode_submit = SampleStage(submit_window_, now);
    p.encode_latency = SampleStage(encode_window_, now);
    p.tick = SampleStage(tick_window_, now);
    p.webcam_convert = SampleStage(webcam_convert_window_, now);
    p.webcam_upload_gpu = SampleStage(webcam_upload_gpu_window_, now);
    p.preview_copy = SampleStage(preview_copy_window_, now);
    p.mux_process = SampleStage(mux_window_, now);
    p.mux_queue_delay = SampleStage(mux_queue_delay_window_, now);

    p.dropped_coalesced = dropped_coalesced_;
    p.dropped_cfr = dropped_cfr_;
    p.dropped_backpressure = dropped_backpressure_;
    p.duplicated_frames = frames_duplicated_;
    p.slot_stalls = slot_stalls_;
    p.queue_saturation_events = queue_saturation_events_;
    return p;
}

PerfSessionSummary PipelineDiagnosticsAggregator::BuildPerfSummary() const {
    std::lock_guard lk(mutex_);
    PerfSessionSummary s;
    s.acquire = SummarizeStage(acquire_hist_);
    s.composition_cpu = SummarizeStage(compositor_hist_);
    s.composition_gpu = SummarizeStage(composition_gpu_hist_);
    s.hdr_tonemap_gpu = SummarizeStage(hdr_tonemap_gpu_hist_);
    s.rgb_to_yuv_cpu = SummarizeStage(vpblt_hist_);
    s.rgb_to_yuv_gpu = SummarizeStage(rgb_to_yuv_gpu_hist_);
    s.encode_submit = SummarizeStage(submit_hist_);
    s.encode_latency = SummarizeStage(encode_hist_);
    s.tick = SummarizeStage(tick_hist_);
    s.webcam_convert = SummarizeStage(webcam_convert_hist_);
    s.webcam_upload_gpu = SummarizeStage(webcam_upload_gpu_hist_);
    s.preview_copy = SummarizeStage(preview_copy_hist_);
    s.mux_process = SummarizeStage(mux_hist_);
    s.mux_queue_delay = SummarizeStage(mux_queue_delay_hist_);

    s.dropped_coalesced = dropped_coalesced_;
    s.dropped_cfr = dropped_cfr_;
    s.dropped_backpressure = dropped_backpressure_;
    s.duplicated_frames = frames_duplicated_;
    s.slot_stalls = slot_stalls_;
    s.queue_saturation_events = queue_saturation_events_;
    s.encoder_init = encoder_init_;
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

    // A sustained video/audio media-duration skew means the timeline is diverging
    // (the honesty signal for a starving-encoder resync). Calm: only above threshold.
    const bool skew_significant = s.duration_skew_availability == MetricAvailability::Available &&
                                  s.duration_skew_ms > thresholds_.duration_skew_warn_ms;

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
    } else if (skew_significant) {
        // Not a single-stage bottleneck — the timeline itself is diverging. Report it
        // calmly without pinning a stage as the root.
        bottleneck = PipelineBottleneck::None;
        reason = "Video and audio durations diverging";
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
               (drops_rising && problem_drops > 0) || skew_significant) {
        health = PipelineHealth::Warning;
    } else {
        health = PipelineHealth::Good;
    }

    return bottleneck;
}

} // namespace recorder_core
