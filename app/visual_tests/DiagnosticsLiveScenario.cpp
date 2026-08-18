#include "visual_tests/DiagnosticsLiveScenario.h"

namespace exosnap::visual {
namespace {

using recorder_core::MetricAvailability;
using recorder_core::RecordingDiagnosticsSnapshot;

// A 60 fps 2560x1440 AV1 recording that is going well. Every scenario below
// starts here and breaks exactly one thing, so a capture difference is the
// scenario's subject and not fixture drift.
RecordingDiagnosticsSnapshot Baseline() {
    RecordingDiagnosticsSnapshot s;
    s.valid = true;
    s.session_generation = 1;
    s.lifecycle = recorder_core::DiagnosticsLifecycle::Recording;
    s.elapsed_seconds = 184.0;
    s.health = recorder_core::PipelineHealth::Good;
    s.bottleneck = recorder_core::PipelineBottleneck::None;

    s.capture.target_fps = 60.0;
    s.capture.actual_fps = 59.98;
    s.capture.frames_captured = 11040;
    s.capture.frames_emitted = 11038;
    s.capture.frames_dropped_coalesced = 622;
    s.capture.source_type = recorder_core::CaptureSourceType::Display;
    s.capture.present_cadence_availability = MetricAvailability::Available;
    s.capture.source_present_interval_ms = 8.33;
    s.capture.source_present_jitter_ms = 1.2;
    s.capture.source_coalesce_ratio = 1.4;
    s.capture.acquire_availability = MetricAvailability::Available;
    s.capture.acquire_latest_ms = 0.8;
    s.capture.acquire_average_ms = 0.7;
    s.capture.acquire_peak_ms = 2.4;

    s.compositor.active = true;
    s.compositor.latest_ms = 0.6;
    s.compositor.average_ms = 0.6;
    s.compositor.peak_ms = 1.9;
    s.compositor.frames_composed = 11038;

    s.video_encoder.codec = recorder_core::VideoCodec::Av1;
    s.video_encoder.width = 2560;
    s.video_encoder.height = 1440;
    s.video_encoder.frames_submitted = 11038;
    s.video_encoder.frames_encoded = 11036;
    s.video_encoder.latest_ms = 3.0;
    s.video_encoder.average_ms = 2.9;
    s.video_encoder.peak_ms = 4.8;
    s.video_encoder.p50_ms = 2.8;
    s.video_encoder.p99_ms = 3.1;
    s.video_encoder.output_fps = 59.97;

    s.encoder_init.valid = true;
    s.encoder_init.codec = recorder_core::VideoCodec::Av1;
    s.encoder_init.preset = recorder_core::NvencPreset::P6;
    s.encoder_init.rc_mode = recorder_core::RateControlMode::ConstantQuality;
    s.encoder_init.cq = 17;
    s.encoder_init.gop_length = 120;

    s.video_timing.availability = MetricAvailability::Available;
    s.video_timing.tick_p50_ms = 4.2;
    s.video_timing.tick_p99_ms = 6.1;
    s.video_timing.tick_peak_ms = 9.0;
    s.video_timing.budget_ms = 16.67;

    s.audio.active = true;
    s.audio.sample_rate = 48000;
    s.audio.channels = 2;
    s.audio.track_count = 1;
    s.audio.codec = recorder_core::AudioCodec::Opus;
    s.audio.packets_encoded = 9200;
    s.audio.bytes_encoded = 3'400'000;

    s.av_drift_availability = MetricAvailability::Available;
    s.av_drift_ms = 0.4;
    s.av_drift_raw_ms = 0.4;
    s.peak_av_drift_availability = MetricAvailability::Available;
    s.peak_av_drift_ms = 1.7;

    s.mux.packets_processed = 20240;
    s.mux.bytes_written = 1'610'612'736;
    s.mux.throughput_mib_s = 92.0;
    s.mux.segment_count = 1;

    s.disk.bytes_written = 1'610'612'736;
    s.disk.throughput_mib_s = 92.0;
    s.disk.latest_write_ms = 0.4;
    s.disk.average_write_ms = 0.3;
    s.disk.peak_write_ms = 2.2;
    s.disk.output_target = "D:\\";
    s.disk_fill_eta_seconds = 8.0 * 3600.0;

    s.split.split_supported = true;
    s.split.current_segment = 1;
    return s;
}

// Present diagnostics as they look when the user HAS opted in and is running
// elevated. Off in every other scenario, which is the shipped default.
void WithPresentObserved(RecordingDiagnosticsSnapshot& s, recorder_core::PresentMode mode, bool tearing) {
    s.capture.present_mode_availability = MetricAvailability::Available;
    s.capture.source_present_mode = mode;
    s.capture.source_tearing = tearing;
}

} // namespace

RecordingDiagnosticsSnapshot MakeDiagnosticsLiveSnapshot(const QString& kind) {
    if (kind == QLatin1String("idle"))
        return {};

    RecordingDiagnosticsSnapshot s = Baseline();

    if (kind == QLatin1String("healthy")) {
        WithPresentObserved(s, recorder_core::PresentMode::IndependentFlip, /*tearing=*/false);
        return s;
    }
    if (kind == QLatin1String("present-unavailable")) {
        // Deliberately identical to `healthy` except for the present gate, so a
        // side-by-side capture shows exactly what the opt-in buys and nothing
        // else moves.
        return s;
    }
    if (kind == QLatin1String("encoder")) {
        s.health = recorder_core::PipelineHealth::Warning;
        s.bottleneck = recorder_core::PipelineBottleneck::VideoEncoder;
        s.bottleneck_reason = "Encoder latency is approaching the frame budget.";
        s.video_encoder.latest_ms = 14.8;
        s.video_encoder.average_ms = 13.1;
        s.video_encoder.peak_ms = 18.2;
        s.video_encoder.p50_ms = 12.9;
        s.video_encoder.p99_ms = 15.9;
        s.video_encoder.backlog = 3;
        s.video_encoder.frames_encoded = 10980;
        s.capture.frames_dropped_backpressure = 41;
        s.capture.actual_fps = 57.4;
        return s;
    }
    if (kind == QLatin1String("disk")) {
        s.health = recorder_core::PipelineHealth::Warning;
        s.bottleneck = recorder_core::PipelineBottleneck::Disk;
        s.bottleneck_reason = "Write latency is above the sustained output rate.";
        s.disk.throughput_mib_s = 21.0;
        s.disk.latest_write_ms = 48.0;
        s.disk.average_write_ms = 31.0;
        s.disk.peak_write_ms = 190.0;
        s.disk.write_failures = 1;
        s.disk_fill_eta_seconds = 900.0;
        s.mux.throughput_mib_s = 21.0;
        return s;
    }
    if (kind == QLatin1String("judder")) {
        s.health = recorder_core::PipelineHealth::Warning;
        s.bottleneck = recorder_core::PipelineBottleneck::Capture;
        s.bottleneck_reason = "The source is presenting irregularly.";
        WithPresentObserved(s, recorder_core::PresentMode::Composed, /*tearing=*/true);
        s.capture.source_present_jitter_ms = 7.9;
        s.capture.actual_fps = 58.1;
        return s;
    }
    if (kind == QLatin1String("degraded")) {
        // The pipeline itself is fine -- the recording keeps running and the file
        // keeps growing (ADR 0046). The audio tile carries the notice on its own.
        s.audio.degraded_sources = 1;
        s.audio.source_degraded = true;
        s.audio.source_degraded_occurred = true;
        return s;
    }
    if (kind == QLatin1String("paused")) {
        s.lifecycle = recorder_core::DiagnosticsLifecycle::Paused;
        return s;
    }
    if (kind == QLatin1String("split")) {
        s.split.split_pending = true;
        s.split.completed_segments = 2;
        s.split.current_segment = 3;
        s.split.last_trigger = recorder_core::DiagnosticsSplitTrigger::AutomaticDuration;
        s.split.last_finalize_ms = 42.0;
        s.mux.segment_count = 3;
        s.mux.split_transitions = 2;
        s.mux.finalizations = 2;
        return s;
    }
    if (kind == QLatin1String("post")) {
        s.lifecycle = recorder_core::DiagnosticsLifecycle::Completed;
        s.duration_skew_availability = MetricAvailability::Available;
        s.duration_skew_ms = 12.0;
        return s;
    }
    return {};
}

} // namespace exosnap::visual
