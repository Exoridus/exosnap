#pragma once

// Frontend-neutral A/B benchmark metric contract.
//
// The Widgets and the Qt Quick frontend drive the SAME recorder engine through the
// SAME orchestration path (auto_record::RunAutoRecordOnCoordinator). What differs is
// the presentation path, and therefore what a "preview frame" even means on each
// side. This header is where that difference is written down once, so a report
// generator can never silently compare two numbers that were not measured at the
// same point.
//
// Nothing here depends on Qt Widgets or Qt Quick: the Widgets target, the Quick
// target and any offline tooling all link the same definitions.

#include <cstdint>
#include <optional>
#include <string>

namespace exosnap::benchmark {

// How a metric may be compared between the two frontends.
enum class Comparability : uint8_t {
    // Same producer, same probe point. A delta is a real behavioural difference.
    // Everything sourced from engine is in this class: both frontends run
    // one identical engine with one identical configuration.
    Identical,

    // Both frontends measure it and the meaning is analogous, but the probe sits
    // at a structurally different place. A delta is evidence, not proof; the
    // report must state the probe difference next to the number.
    Approximate,

    // Only one frontend can produce this. Never subtract; report side by side.
    FrontendOnly,
};

[[nodiscard]] const char* ComparabilityName(Comparability value) noexcept;

// One measured value plus the contract under which it may be read. `value` is
// unset when the run could not measure it — an absent metric is reported as
// absent, never as a zero that reads like "measured, and it was zero".
struct Metric {
    std::optional<double> value;
    Comparability comparability = Comparability::Approximate;
    // Where the number comes from, in one sentence. Emitted into the report so a
    // reader never has to consult the source to know what was timed.
    std::string probe;
};

[[nodiscard]] Metric MakeMetric(double value, Comparability comparability, std::string probe);
[[nodiscard]] Metric UnavailableMetric(Comparability comparability, std::string probe);

// ---------------------------------------------------------------------------
// Preview / UI metrics — the frontend-specific half.
//
// Filled by each frontend from its own instrumentation:
//   Widgets → DxgiPreviewPerformanceSnapshot (dedicated DXGI render thread,
//             native child HWND, presents a preview quad into its own swap chain)
//   Quick   → PreviewMetricsSnapshot (Qt scene-graph render thread, one top-level
//             HWND, composites the preview item together with the whole UI)
//
// The two structurally different probe points are exactly why `frame_cadence_*`
// is Approximate while `source_*` and `submit_*` are Identical: the latter measure
// the shared preview transport (keyed-mutex acquire on the producer's texture),
// which is the same mechanism on both sides.
// ---------------------------------------------------------------------------
struct PreviewMetrics {
    // Frames the frontend's presentation path produced in the measured window.
    // APPROXIMATE: a Widgets "frame" is one swap-chain Present of the preview quad;
    // a Quick "frame" is one scene-graph render of the entire window.
    Metric frames_presented;

    // Frames taken off the shared preview texture. IDENTICAL: both sides consume
    // the same producer through the same keyed mutex.
    Metric source_frames_consumed;

    // Keyed-mutex AcquireSync(0) failures — the consumer asked and there was
    // nothing new to take.
    //
    // APPROXIMATE, despite being the same mutex and the same non-blocking acquire
    // on both sides: the count is one per *attempt*, so it scales with how often
    // the consumer looks, and that rate is exactly what differs structurally
    // between a dedicated preview render thread and a vsync-driven scene graph. A
    // measured idle run made this concrete — the Quick side reported 5403 misses
    // against 5408 renders while consuming 5 frames, i.e. the number was a
    // restatement of its own render count, not a property of the transport. Only
    // the ratio to frames_presented carries meaning, and the report must not
    // subtract the raw counts.
    Metric mutex_misses;

    // Presentation cadence. APPROXIMATE for the same reason as frames_presented.
    Metric frame_cadence_fps;
    Metric frame_ms_p50;
    Metric frame_ms_p95;
    Metric frame_ms_p99;
    Metric frame_ms_max;

    // Rate and jitter at which frames ARRIVE AT THIS CONSUMER off the shared
    // texture.
    //
    // APPROXIMATE, not identical, despite one producer feeding both. The rate is
    // observed on the consume side, so a consumer that looks less often than the
    // producer delivers simply does not see every frame. The Superposition
    // campaign made that unmistakable: against one identical workload the Widgets
    // side reported 29.96 fps and the Quick side 61.75 fps — not because the
    // engine fed them differently, but because one polls at ~62 Hz and the other
    // at 144 Hz. Read as "what the preview actually got", never as "what the
    // engine produced".
    Metric source_delivery_fps;
    Metric source_interval_ms_p95;
    Metric source_interval_ms_p99;

    // GPU submit duration for the preview copy/convert. IDENTICAL in what it
    // brackets (the consumer's copy off the shared texture), even though the
    // surrounding render loop differs.
    Metric submit_us_p50;
    Metric submit_us_p95;
    Metric submit_us_p99;

    // Native child windows under the top-level window at sample time. The whole
    // point of the Quick migration, and structurally frontend-specific: the
    // Widgets preview IS a child HWND, the Quick preview is a scene-graph item.
    Metric child_hwnd_count;

    // Presentation work per frame that actually arrived: frames_presented /
    // source_frames_consumed. APPROXIMATE, because its numerator is — a Widgets
    // "present" and a Quick "window render" are not the same unit of work. What
    // it does compare honestly is how much of each frontend's presentation work
    // was spent redrawing a picture that had not changed. 1.0 is the floor.
    Metric render_amplification;

    // How the preview's redraw was scheduled. FRONTEND-ONLY: the Widgets preview
    // presents from a dedicated render thread on a fixed interval and has no
    // scheduling gate to measure, while the Quick preview is driven by the
    // producers' per-frame publish edge through PreviewUpdateScheduler.
    //
    // publish_signals >= scene_update_requests is the gate doing its job: the
    // difference is publishes that found a wake-up already in flight and
    // correctly added no second render.
    Metric preview_publish_signals;
    Metric preview_scene_update_requests;

    // The rest of the consume funnel. IDENTICAL: these count the shared
    // transport, not the frontend's own presentation work.
    Metric consumer_acquires;
    Metric consumer_acquire_abandoned;
    Metric consumer_conversion_failures;

    // The producer's own cadence and the age of the debt it created, on the same
    // clock and the same window as everything above. Without the first, a long
    // arrival interval cannot be told from a quiet source; without the second, a
    // last-value slot hides a stall behind a young newest frame.
    Metric publish_interval_ms_p50;
    Metric publish_interval_ms_p95;
    Metric publish_interval_ms_p99;
    Metric publish_interval_ms_max;
    Metric presentation_debt_ms_p50;
    Metric presentation_debt_ms_p95;
    Metric presentation_debt_ms_p99;
    Metric presentation_debt_ms_max;
    Metric source_interval_ms_max;
};

// ---------------------------------------------------------------------------
// Recording metrics — the fully comparable half.
//
// Every field here is produced by engine from one identical configuration,
// so each is Comparability::Identical by construction. This is the half that
// answers the questions the A/B exists for: does the Quick frontend cost the
// recording anything?
// ---------------------------------------------------------------------------
struct RecordingMetrics {
    Metric target_fps;

    // The engine's own rate, computed over its LAST publish interval. Useful as a
    // steady-state reading, useless as a run summary — on a run that ends with a
    // quiet second it reads 0 while thousands of frames were emitted.
    Metric actual_fps;

    // Emitted frames in the measured window divided by that window. This is the
    // run-level rate, and the one a reader should compare.
    Metric measured_window_emitted_fps;

    Metric frames_captured;
    Metric frames_emitted;
    Metric frames_duplicated;

    // The four drop categories kept apart. Collapsing them hides the distinction
    // the engine deliberately draws between benign pacing loss and real picture
    // loss (see pipeline_diagnostics.h: frames_dropped_problematic()).
    Metric frames_dropped_coalesced;
    Metric frames_dropped_cfr;
    Metric frames_dropped_backpressure;
    Metric frames_dropped_processing_failure;
    Metric frames_dropped_problematic;

    // Capture-side acquire+copy CPU duration.
    Metric acquire_average_ms;
    Metric acquire_peak_ms;

    // Encoder and mux pressure.
    Metric encoder_queue_depth;
    Metric encoder_latency_ms;
    Metric mux_queue_depth;
    Metric audio_frames_dropped;

    // A/V alignment at the end of the run.
    Metric av_drift_ms;

    // The producer half of the preview path, as a funnel. The consumer half is
    // in PreviewMetrics; the two only answer the question together, because a
    // preview that shows nothing looks identical from either end alone.
    Metric preview_tap_frames_seen;
    Metric preview_tap_gate_passes;
    Metric preview_tap_shared_texture_ready;
    Metric preview_tap_publish_attempts;
    Metric preview_tap_publish_successes;
    Metric preview_tap_publish_mutex_misses;
    Metric preview_tap_publish_abandoned;
    Metric preview_tap_publish_failures;
    Metric preview_tap_publish_release_failures;
    Metric preview_tap_published_edges;
};

// Process-level cost, measured identically on both sides (Win32 GetProcessTimes /
// working set over the measured window).
struct ProcessMetrics {
    Metric cpu_percent;
    Metric peak_working_set_mb;
    Metric working_set_mb;
};

} // namespace exosnap::benchmark
