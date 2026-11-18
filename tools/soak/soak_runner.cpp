#include "soak_runner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace exosnap::soak {

using exosnap::engine::MetricAvailability;
using exosnap::engine::PipelineHealth;

SoakRunner::SoakRunner(SoakThresholds thresholds, IProcessSampler& sampler, std::string jsonl_path,
                       std::function<void()> abort_callback)
    : thresholds_(thresholds), policy_(thresholds), sampler_(sampler), jsonl_path_(std::move(jsonl_path)),
      abort_callback_(std::move(abort_callback)) {
    if (!jsonl_path_.empty()) {
        jsonl_.open(jsonl_path_, std::ios::binary);
    }
}

SoakRunner::~SoakRunner() {
    Stop();
}

void SoakRunner::OnStats(const exosnap::engine::SessionStats& stats) {
    std::lock_guard lk(latch_mutex_);
    last_stats_ = stats;
    have_stats_ = true;
}

void SoakRunner::OnDiagnostics(const exosnap::engine::RecordingDiagnosticsSnapshot& diag) {
    std::lock_guard lk(latch_mutex_);
    last_diag_ = diag;
    have_diag_ = true;
}

void SoakRunner::ConfigureStorageWatch(StorageWatchConfig config, std::function<StorageObservation(double)> observe) {
    if (started_ || !observe || !std::isfinite(config.poll_seconds) || config.poll_seconds <= 0)
        throw std::invalid_argument("Configure the storage watch before starting the sampler");
    storage_config_ = config;
    storage_observer_ = std::move(observe);
}

void SoakRunner::Start(double sample_interval_s) {
    if (started_)
        throw std::logic_error("A soak sampler can only start once");
    if (!std::isfinite(sample_interval_s) || sample_interval_s <= 0)
        throw std::invalid_argument("Soak sample interval must be positive and finite");
    if (storage_config_ && sample_interval_s > storage_config_->poll_seconds)
        throw std::invalid_argument("Soak sampling interval cannot exceed the storage watch interval");
    if (!jsonl_path_.empty() && !jsonl_.is_open())
        throw std::runtime_error("Cannot open soak timeline: " + jsonl_path_);
    started_ = true;
    start_time_ = std::chrono::steady_clock::now();
    stop_.store(false);
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(sample_interval_s));
    thread_ = std::thread([this, interval] {
        try {
            // One sample immediately so even a sub-interval run yields a row.
            SampleOnce();
            std::unique_lock lock(stop_mutex_);
            while (!stop_.load()) {
                auto wait_interval = interval;
                if (storage_config_ && storage_check_) {
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
                    const auto storage_wait =
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(
                            StoragePollDelay(*storage_config_, storage_check_->observation, elapsed)));
                    wait_interval = std::min(wait_interval, storage_wait);
                }
                if (wake_.wait_for(lock, wait_interval, [this] { return stop_.load(); }))
                    break;
                lock.unlock();
                SampleOnce();
                lock.lock();
            }
        } catch (const std::exception& error) {
            ReportFailure(error.what());
        } catch (...) {
            ReportFailure("Unknown soak sampler failure");
        }
    });
}

void SoakRunner::Stop() {
    if (thread_.joinable()) {
        {
            std::lock_guard lock(stop_mutex_);
            stop_.store(true);
        }
        wake_.notify_all();
        thread_.join();
        if (error().empty()) {
            try {
                SampleOnce();
            } catch (const std::exception& error) {
                ReportFailure(error.what());
            } catch (...) {
                ReportFailure("Unknown final soak sample failure");
            }
        }
    }
}

void SoakRunner::SampleOnce() {
    SoakSample s;
    s.unix_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    s.t_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();

    if (storage_config_ &&
        (!storage_check_ || s.t_s - storage_check_->observation.t_s >= storage_config_->poll_seconds)) {
        storage_check_ = CheckStorage(*storage_config_, storage_observer_(s.t_s), storage_check_);
    }
    s.storage = storage_check_;

    int64_t adapter_luid = 0;
    {
        std::lock_guard lock(latch_mutex_);
        if (have_diag_)
            adapter_luid = last_diag_.capture_adapter_luid;
    }
    const ProcessMetrics pm = sampler_.SampleForAdapter(adapter_luid);
    s.process_cpu_percent = pm.process_cpu_percent;
    s.system_cpu_percent = pm.system_cpu_percent;
    s.gpu_utilization_percent = pm.gpu_utilization_percent;
    s.encoder_utilization_percent = pm.encoder_utilization_percent;
    s.process_gpu_memory_bytes = pm.process_gpu_memory_bytes;
    s.rss_bytes = pm.rss_bytes;
    s.private_bytes = pm.private_bytes;
    s.handle_count = pm.handle_count;
    s.gdi_objects = pm.gdi_objects;
    s.user_objects = pm.user_objects;

    {
        std::lock_guard lk(latch_mutex_);
        if (have_diag_) {
            const auto& d = last_diag_;
            s.av_drift_ms = d.av_drift_ms;
            s.av_drift_available = d.av_drift_availability == MetricAvailability::Available;
            s.duration_skew_ms = d.duration_skew_ms;
            s.duration_skew_available = d.duration_skew_availability == MetricAvailability::Available;
            s.frames_captured = d.capture.frames_captured;
            s.frames_emitted = d.capture.frames_emitted;
            s.frames_dropped_coalesced = d.capture.frames_dropped_coalesced;
            s.frames_dropped_cfr = d.capture.frames_dropped_cfr;
            s.frames_dropped_backpressure = d.capture.frames_dropped_backpressure;
            s.frames_dropped_processing_failure = d.capture.frames_dropped_processing_failure;
            s.frames_dropped_ring_eviction = d.capture.frames_dropped_ring_eviction;
            s.encoder_undrained_frames = d.video_encoder.undrained_frames;
            s.encoder_flush_incomplete = d.video_encoder.flush_incomplete;
            s.real_frame_loss = d.real_frame_loss();
            s.frames_duplicated = d.capture.frames_duplicated;
            s.audio_discontinuities = d.audio.discontinuities;
            s.encoder_backlog = d.video_encoder.backlog;
            s.audio_premux_queue_depth = d.audio_queue.current_depth;
            s.disk_write_failures = d.disk.write_failures;
            s.mux_failures = d.mux.failures;
            s.recorder_failed = s.disk_write_failures != 0 || s.mux_failures != 0;
            if (d.valid) {
                s.audio_source_degraded = d.audio.source_degraded;
                s.audio_source_degraded_occurred = d.audio.source_degraded_occurred;
                s.seconds_without_capture = d.capture.seconds_without_capture;
                s.capture_starved = d.capture.capture_starved;
                if (d.audio.discontinuity_availability == MetricAvailability::Available && d.audio.sample_rate != 0) {
                    s.audio_discontinuity_ms_total = 1000.0 * d.audio.discontinuity_frames_total / d.audio.sample_rate;
                    s.audio_discontinuity_ms_longest =
                        1000.0 * d.audio.discontinuity_frames_longest / d.audio.sample_rate;
                }
                for (size_t track = 0; track < d.audio.track_count && track < d.audio.resampler_drain_recorded.size();
                     ++track) {
                    s.resampler_undrained_frames.push_back(
                        d.audio.resampler_drain_recorded[track]
                            ? std::optional(d.audio.resampler_undrained_frames[track])
                            : std::nullopt);
                }
            }
            s.mux_queue_depth = d.video_queue.current_depth;
            s.disk_fill_eta_s = d.disk_fill_eta_seconds;
            s.health_critical = d.health == PipelineHealth::Critical;
            s.bottleneck = d.bottleneck_reason;
        }
        if (have_stats_) {
            // SessionStats carries the authoritative media-duration skew and dup/drop
            // counters; prefer it when diagnostics has not populated skew yet.
            if (!s.duration_skew_available && last_stats_.duration_skew_ms != 0.0) {
                s.duration_skew_ms = last_stats_.duration_skew_ms;
                s.duration_skew_available = true;
            }
            if (s.frames_duplicated == 0)
                s.frames_duplicated = last_stats_.duplicated_video_frames;
        }
    }

    // Test-only deterministic skew ramp (see header).
    if (skew_injection_ms_per_s_ != 0.0) {
        s.duration_skew_ms += skew_injection_ms_per_s_ * s.t_s;
        s.duration_skew_available = true;
    }

    AbortDecision decision;
    {
        std::lock_guard lk(timeline_mutex_);
        timeline_.push_back(s);
        if (jsonl_.is_open()) {
            jsonl_ << SampleToJsonLine(s);
            jsonl_.flush();
            if (!jsonl_)
                throw std::runtime_error("Cannot write soak timeline: " + jsonl_path_);
        }
        if (!aborted_.load()) {
            decision = policy_.Evaluate(timeline_);
        }
    }
    if (decision.verdict == SoakVerdict::Abort)
        PublishAbort(std::move(decision));
}

void SoakRunner::PublishAbort(AbortDecision decision) {
    {
        std::lock_guard lock(abort_mutex_);
        if (aborted_.load())
            return;
        abort_decision_ = std::move(decision);
        aborted_.store(true);
    }
    // The session stop path and callback consumers may read the runner. Never
    // call them while holding the timeline or abort locks.
    if (abort_callback_)
        abort_callback_();
}

void SoakRunner::ReportFailure(const std::string& error) {
    {
        std::lock_guard lock(abort_mutex_);
        if (error_.empty())
            error_ = error;
    }
    PublishAbort({SoakVerdict::Abort, error});
}

AbortDecision SoakRunner::abort_decision() const {
    std::lock_guard lk(abort_mutex_);
    return abort_decision_;
}

std::string SoakRunner::error() const {
    std::lock_guard lock(abort_mutex_);
    return error_;
}

std::vector<SoakSample> SoakRunner::timeline() const {
    std::lock_guard lk(timeline_mutex_);
    return timeline_;
}

SoakSummary SoakRunner::Summarize() const {
    std::lock_guard lk(timeline_mutex_);
    auto summary = SoakMetricsAggregator(thresholds_).Summarize(timeline_);
    if (aborted_.load())
        summary.advisory_verdict = abort_decision();
    return summary;
}

} // namespace exosnap::soak
