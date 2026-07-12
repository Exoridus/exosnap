#pragma once

// SoakRunner — the endurance run-loop.
//
// It latches the latest RecorderSession callback snapshots (thread-safe), samples
// the host process on a fixed cadence, appends one SoakSample per tick to a
// JSON-Lines timeline, and evaluates SoakAbortPolicy against the growing history.
// The engine callbacks and the abort wiring are identical for the real GPU path
// and the synthetic twin — only the sample SOURCE differs, so this loop is fully
// exercised on CI by driving the synthetic session with a fake process sampler.

#include "soak_metrics.h"
#include "soak_process_sampler.h"

#include <recorder_core/pipeline_diagnostics.h>
#include <recorder_core/session_stats.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace exosnap::soak {

class SoakRunner {
  public:
    // `sampler` and `jsonl_path` must outlive the runner. An empty jsonl_path
    // disables the on-disk timeline (in-memory only — used by the unit test).
    SoakRunner(SoakThresholds thresholds, IProcessSampler& sampler, std::string jsonl_path);
    ~SoakRunner();

    // Thread-safe engine callback sinks — pass straight to
    // RecorderSession::SetStatsCallback / SetDiagnosticsCallback (or the synthetic
    // session's equivalents).
    void OnStats(const recorder_core::SessionStats& stats);
    void OnDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& diag);

    // Start/stop the sampler thread. Start records t=0; Stop takes a final sample.
    void Start(double sample_interval_s);
    void Stop();

    // True once the abort policy has fired; `abort_decision()` carries the reason.
    [[nodiscard]] bool aborted() const {
        return aborted_.load();
    }
    [[nodiscard]] AbortDecision abort_decision() const;

    [[nodiscard]] std::vector<SoakSample> timeline() const;
    [[nodiscard]] SoakSummary Summarize() const;

    // Test hook: add `ramp_ms_per_s * t_s` to each sample's duration_skew_ms so the
    // wired abort path can be exercised deterministically without a real stall.
    void SetSkewInjection(double ramp_ms_per_s) {
        skew_injection_ms_per_s_ = ramp_ms_per_s;
    }

  private:
    void SampleOnce();

    SoakThresholds thresholds_;
    SoakAbortPolicy policy_;
    IProcessSampler& sampler_;
    std::string jsonl_path_;
    std::ofstream jsonl_;

    mutable std::mutex latch_mutex_;
    recorder_core::SessionStats last_stats_{};
    recorder_core::RecordingDiagnosticsSnapshot last_diag_{};
    bool have_stats_ = false;
    bool have_diag_ = false;

    mutable std::mutex timeline_mutex_;
    std::vector<SoakSample> timeline_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> aborted_{false};
    mutable std::mutex abort_mutex_;
    AbortDecision abort_decision_;

    std::chrono::steady_clock::time_point start_time_;
    double skew_injection_ms_per_s_ = 0.0;
};

} // namespace exosnap::soak
