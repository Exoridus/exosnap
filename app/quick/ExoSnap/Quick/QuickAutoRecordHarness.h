#pragma once

// The Qt Quick half of the frontend A/B benchmark.
//
// Everything that decides what the numbers mean — the capability gate, the
// committed output/video settings, the target-selection rule, the
// warm-up/measure/stop sequence, the engine metrics, the process sampling and
// the report format — lives in auto_record::RunAutoRecordOnCoordinator and is
// shared verbatim with the Widgets entry point. What remains here is the two
// BenchmarkHooks probes and the window placement, i.e. exactly the parts that
// are genuinely frontend-specific.
//
// Compiled only when EXOSNAP_ENABLE_AUTO_RECORD_HARNESS is defined (see
// EXOSNAP_BUILD_BENCHMARK_HARNESS in app/CMakeLists.txt).

#include "auto_record/AutoRecordHarness.h"

class QCoreApplication;
class QQuickWindow;

namespace exosnap::quick {

class QuickApplication;

// Places the window per benchmark::ResolveHarnessWindowPlacement, waits for the
// real asynchronous capability probe to bring the coordinator to Ready, selects
// the requested capture target through the same path a source-picker click
// takes, and hands the coordinator to the shared drive loop.
//
// Returns the process exit code. `window` may be null (the shared loop still
// runs; the child-HWND metric is then reported as unavailable).
[[nodiscard]] int RunQuickAutoRecord(QCoreApplication& app, QuickApplication& application, QQuickWindow* window,
                                     const auto_record::AutoRecordOptions& options);

} // namespace exosnap::quick
