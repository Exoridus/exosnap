#pragma once

#include <QString>

#include <mutex>
#include <vector>

namespace exosnap::diagnostics {

struct StartupTraceEntry {
    QString label;
    qint64 elapsed_ms = 0;
};

// PERF-MEASURE: process-global startup milestone collector, a companion to
// StartupClock. The same StartupClock().elapsed() reads that are logged as perf
// lines are additionally recorded here (label + ms) so Logs can render a Startup
// table and the support bundle can carry startup-trace.txt — making startup
// regressions visible instead of buried in log lines.
//
// Records are de-duplicated by label (each milestone is logged once; a second
// record() for the same label is ignored) and kept in first-seen order. record()
// runs on the main thread at the milestones; entries() is read on the main thread
// (Logs page) and once at bundle time — a mutex keeps it safe regardless.
class StartupTrace {
  public:
    static StartupTrace& instance() {
        static StartupTrace t;
        return t;
    }

    void record(const QString& label, qint64 elapsed_ms) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& e : entries_) {
            if (e.label == label)
                return; // milestone already recorded; keep the first reading
        }
        entries_.push_back({label, elapsed_ms});
    }

    [[nodiscard]] std::vector<StartupTraceEntry> entries() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return entries_;
    }

    // Test support.
    void resetForTesting() {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<StartupTraceEntry> entries_;
};

// Deterministic text rendering for the bundle entry startup-trace.txt.
inline QString FormatStartupTrace(const std::vector<StartupTraceEntry>& entries) {
    QString out;
    for (const auto& e : entries) {
        out += QStringLiteral("%1\t%2 ms\n").arg(e.label).arg(e.elapsed_ms);
    }
    return out;
}

} // namespace exosnap::diagnostics
