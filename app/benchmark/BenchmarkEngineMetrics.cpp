#include "BenchmarkEngineMetrics.h"

namespace exosnap::benchmark {
namespace {

constexpr Comparability kSame = Comparability::Identical;

Metric Counter(bool valid, uint64_t value, const char* probe) {
    return valid ? MakeMetric(static_cast<double>(value), kSame, probe) : UnavailableMetric(kSame, probe);
}

Metric Value(bool valid, double value, const char* probe) {
    return valid ? MakeMetric(value, kSame, probe) : UnavailableMetric(kSame, probe);
}

// A metric the engine itself marks Unavailable must stay unavailable here. The
// engine's rule — "metrics that cannot be measured are marked Unavailable rather
// than reported as a fake zero" — would otherwise be undone at the report boundary.
Metric Gated(bool valid, recorder_core::MetricAvailability availability, double value, const char* probe) {
    const bool measured = valid && availability == recorder_core::MetricAvailability::Available;
    return measured ? MakeMetric(value, kSame, probe) : UnavailableMetric(kSame, probe);
}

} // namespace

RecordingMetrics RecordingMetricsFromSnapshot(const recorder_core::RecordingDiagnosticsSnapshot& terminal,
                                              const recorder_core::RecordingDiagnosticsSnapshot& baseline,
                                              double measured_seconds) {
    const bool valid = terminal.valid;
    const recorder_core::CaptureDiagnostics& capture = terminal.capture;
    const recorder_core::EncoderDiagnostics& encoder = terminal.video_encoder;

    // Only difference against a baseline from the SAME session. A baseline from a
    // previous cycle would produce counters that look small because they were
    // subtracted against a larger number, i.e. a run that appears to have dropped
    // nothing precisely because it dropped more.
    const bool differenced = baseline.valid && baseline.session_generation == terminal.session_generation;
    const recorder_core::CaptureDiagnostics& base = baseline.capture;
    const char* const window = differenced ? " over the measured window" : " over the whole session (no baseline)";

    // Saturating subtraction: a counter that somehow moved backwards must not wrap
    // into an astronomically large "measurement".
    const auto delta = [&](uint64_t now, uint64_t then) -> uint64_t {
        if (!differenced)
            return now;
        return now >= then ? now - then : 0;
    };

    RecordingMetrics metrics;
    metrics.target_fps = Value(valid, capture.target_fps, "recorder_core CaptureDiagnostics::target_fps");
    metrics.actual_fps = Value(valid, capture.actual_fps,
                               "recorder_core CaptureDiagnostics::actual_fps — the engine's rate over its LAST "
                               "publish interval, not a run average");

    const uint64_t emitted = delta(capture.frames_emitted, base.frames_emitted);
    metrics.measured_window_emitted_fps =
        (valid && measured_seconds > 0.0)
            ? MakeMetric(static_cast<double>(emitted) / measured_seconds, kSame,
                         "derived: emitted frames in the measured window / that window's length")
            : UnavailableMetric(kSame, "derived: needs a valid snapshot and a non-zero measured window");

    metrics.frames_captured = Counter(valid, delta(capture.frames_captured, base.frames_captured),
                                      "recorder_core CaptureDiagnostics::frames_captured (frames the capture "
                                      "backend actually produced)");
    metrics.frames_emitted = Counter(valid, emitted, "recorder_core CaptureDiagnostics::frames_emitted");
    metrics.frames_duplicated = Counter(valid, delta(capture.frames_duplicated, base.frames_duplicated),
                                        "recorder_core CaptureDiagnostics::frames_duplicated (CFR hold)");

    metrics.frames_dropped_coalesced =
        Counter(valid, delta(capture.frames_dropped_coalesced, base.frames_dropped_coalesced),
                "recorder_core: newer frame replaced an unconsumed one");
    metrics.frames_dropped_cfr = Counter(valid, delta(capture.frames_dropped_cfr, base.frames_dropped_cfr),
                                         "recorder_core: scheduled CFR tick had nothing to encode yet");
    metrics.frames_dropped_backpressure =
        Counter(valid, delta(capture.frames_dropped_backpressure, base.frames_dropped_backpressure),
                "recorder_core: all encoder input slots in flight");
    metrics.frames_dropped_processing_failure =
        Counter(valid, delta(capture.frames_dropped_processing_failure, base.frames_dropped_processing_failure),
                "recorder_core: frame conversion failed");
    // The engine's own definition of a drop that cost real picture — backpressure
    // plus processing failure, excluding the two benign pacing categories. Using
    // the engine's accessor rather than re-adding the fields keeps this report and
    // every user-facing drop surface on one definition.
    metrics.frames_dropped_problematic =
        Counter(valid, delta(capture.frames_dropped_problem(), base.frames_dropped_problem()),
                "recorder_core CaptureDiagnostics::frames_dropped_problem() (backpressure + processing failure)");

    // Stated once, on the metrics whose meaning depends on it.
    for (Metric* counter :
         {&metrics.frames_captured, &metrics.frames_emitted, &metrics.frames_duplicated,
          &metrics.frames_dropped_coalesced, &metrics.frames_dropped_cfr, &metrics.frames_dropped_backpressure,
          &metrics.frames_dropped_processing_failure, &metrics.frames_dropped_problematic}) {
        counter->probe += window;
    }

    metrics.acquire_average_ms = Gated(valid, capture.acquire_availability, capture.acquire_average_ms,
                                       "recorder_core: capture acquire+copy CPU duration, average");
    metrics.acquire_peak_ms = Gated(valid, capture.acquire_availability, capture.acquire_peak_ms,
                                    "recorder_core: capture acquire+copy CPU duration, peak");

    metrics.encoder_queue_depth =
        Counter(valid, encoder.backlog, "recorder_core EncoderDiagnostics::backlog (submitted - encoded)");
    metrics.encoder_latency_ms =
        Value(valid, encoder.p99_ms, "recorder_core EncoderDiagnostics::p99_ms (submit -> bitstream ready)");
    // A running peak, not a sum: differencing it would be meaningless, so it is
    // reported as the session peak and labelled as such.
    metrics.mux_queue_depth =
        Counter(valid, terminal.video_queue.peak_depth, "recorder_core: post-encode mux queue peak depth (session)");
    metrics.audio_frames_dropped =
        Gated(valid, terminal.audio.discontinuity_availability,
              static_cast<double>(differenced && terminal.audio.discontinuities >= baseline.audio.discontinuities
                                      ? terminal.audio.discontinuities - baseline.audio.discontinuities
                                      : terminal.audio.discontinuities),
              "recorder_core AudioDiagnostics::discontinuities");
    metrics.audio_frames_dropped.probe += window;

    metrics.av_drift_ms = Gated(valid, terminal.av_drift_availability, terminal.av_drift_ms,
                                "recorder_core: residual A/V drift as it lands in the file");
    return metrics;
}

} // namespace exosnap::benchmark
