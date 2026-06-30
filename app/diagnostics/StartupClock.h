#pragma once

#include <QElapsedTimer>

namespace exosnap::diagnostics {

// PERF-MEASURE: process-global startup clock for start→milestone latency.
//
// Started exactly once as the very first statement of main() (before QApplication),
// then read at two milestones to log a clean before/after baseline:
//   * first MainWindow paint  → "window interactive" (AppLog perf "first-paint N ms")
//   * DXGI preview goes live   → "preview live"        (AppLog perf "preview-live N ms")
//
// Header-only on purpose: an inline function's function-local static yields a single
// shared instance across every translation unit that includes it, so no extra .cpp /
// CMake registration is needed. start() runs on the main thread before exec(); the
// later elapsed() reads also run on the main thread (paint / preview-start), so no
// synchronization is required.
inline QElapsedTimer& StartupClock() {
    static QElapsedTimer clock;
    return clock;
}

} // namespace exosnap::diagnostics
