#pragma once

// Engine diagnostics → neutral benchmark metrics.
//
// Kept apart from BenchmarkReport.h so the metric contract itself stays free of
// recorder_core. Everything this file produces is Comparability::Identical by
// construction: both frontends drive one engine, configured identically by the
// shared orchestration path, and read the terminal snapshot from the same place.

#include "BenchmarkMetrics.h"

#include <exosnap/engine/pipeline_diagnostics.h>

namespace exosnap::benchmark {

// `terminal.valid == false` (idle, or the session never produced diagnostics)
// yields all-unavailable metrics rather than a run that looks like it recorded
// perfectly and dropped nothing.
//
// `baseline` is the snapshot taken when the warm-up ended. The engine's counters
// are cumulative for the whole session, so without subtracting a baseline every
// counter in the report would silently include the warm-up — the very interval
// the warm-up exists to keep out of the measurement. Pass a default-constructed
// (invalid) baseline to report raw session totals; the probe text says which of
// the two a number is.
//
// Gauges and rates are NOT differenced: `actual_fps` is the engine's rate over
// its last publish interval, `mux_queue_depth` is a running peak, and the
// acquire timings are rolling-window statistics. Subtracting those would be
// arithmetic on quantities that are not sums. `measured_window_emitted_fps` is
// the honest run-level rate, derived from the differenced emitted count.
[[nodiscard]] RecordingMetrics
RecordingMetricsFromSnapshot(const exosnap::engine::RecordingDiagnosticsSnapshot& terminal,
                             const exosnap::engine::RecordingDiagnosticsSnapshot& baseline, double measured_seconds);

} // namespace exosnap::benchmark
