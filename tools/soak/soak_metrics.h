// soak_metrics.h — pure, host-agnostic soak analysis.
//
// This header holds the ENTIRE decision surface of the endurance ("soak") tool
// as pure data + pure resolvers: no WinAPI, no FFmpeg, no engine linkage, no file
// I/O. The `exosnap-soak` host tool fills a `SoakSample` per second from the
// RecorderSession callbacks plus its own process sampler, then feeds the growing
// history to `SoakAbortPolicy` (should the run stop early?) and, at the end, to
// `SoakMetricsAggregator` (what did the run look like?). Keeping this pure is the
// point: RAM/handle growth is a property of the HOST process, never the engine,
// and the abort/report math must be unit-testable without a GPU or a real file.
//
// Thresholds here are ADVISORY for 0.10 (see docs/dev/soak-and-recovery-drills.md):
// they bound obviously-diverging runs so a broken 2 h soak stops in minutes rather
// than hours, but crossing one is not a release gate.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::soak {

// One metric sample, one row of the JSON-Lines timeline the tool writes. Every
// field is either straight from a RecorderSession callback (SessionStats /
// RecordingDiagnosticsSnapshot) or from the host process sampler (rss/handles/
// GUI objects). Engine enums are reduced to booleans here so this stays decoupled
// from recorder_core.
struct SoakSample {
    double t_s = 0.0; // seconds since soak start (wall clock)

    double av_drift_ms = 0.0;         // clock drift; +ve = audio leads. Only meaningful when available.
    bool av_drift_available = false;  // false until a device-backed audio track reports timing
    double duration_skew_ms = 0.0;    // |video media time - audio media time|
    bool duration_skew_available = false;

    uint64_t frames_captured = 0; // cumulative
    uint64_t frames_emitted = 0;  // cumulative (CFR output rate)
    uint64_t frames_dropped_coalesced = 0;
    uint64_t frames_dropped_cfr = 0;
    uint64_t frames_dropped_backpressure = 0;
    uint64_t frames_duplicated = 0;
    uint64_t audio_discontinuities = 0;

    uint32_t mux_queue_depth = 0;
    double disk_fill_eta_s = -1.0; // <0 == unavailable

    uint64_t rss_bytes = 0;     // WorkingSetSize
    uint64_t private_bytes = 0; // PrivateUsage (commit)
    uint32_t handle_count = 0;
    uint32_t gdi_objects = 0;
    uint32_t user_objects = 0;

    bool health_critical = false; // PipelineHealth::Critical
    bool recorder_failed = false; // a RecorderResult failure was observed
    std::string bottleneck;       // advisory, human-readable

    // Sum of the three drop counters (a convenience; not serialized separately).
    [[nodiscard]] uint64_t total_dropped() const {
        return frames_dropped_coalesced + frames_dropped_cfr + frames_dropped_backpressure;
    }
};

// Advisory abort budgets. Defaults are the 0.10 starting values; the tool exposes
// overrides on the command line.
struct SoakThresholds {
    // A/V clock-drift budget (ms). Start value ≤ 20 ms / 2 h. Considered only when
    // the sample marks drift available.
    double av_drift_abort_ms = 20.0;

    // Duration-skew budget (ms). Aborts only when over budget AND still growing across
    // the sustained window (a single spike from a transient stall must not abort).
    double duration_skew_abort_ms = 20.0;

    // Drop ratio (dropped / max(1, emitted)). Aborts when sustained over this.
    double drop_ratio_abort = 0.02; // 2 %

    // Leak SLOPE thresholds (least-squares over the whole run). Advisory; slopes are
    // the criterion, not an absolute ceiling, because RSS has legitimate non-leak
    // growth (driver caches, heap fragmentation).
    double rss_slope_abort_bytes_per_s = 262144.0; // 256 KiB/s sustained
    double handle_slope_abort_per_s = 1.0;         // 1 handle/s sustained

    // A condition must hold across this many trailing samples to count as sustained
    // (defeats single-sample spikes). At ~1 Hz this is ~N seconds.
    int sustained_samples = 30;

    // Slope-based aborts are suppressed until at least this many samples exist, so a
    // short warm-up transient is not mistaken for a leak.
    int min_slope_samples = 120;
};

enum class SoakVerdict { Continue, Abort };

struct AbortDecision {
    SoakVerdict verdict = SoakVerdict::Continue;
    std::string reason; // empty on Continue; a one-line explanation on Abort
};

// Pure abort resolver. `Evaluate` inspects the full chronological history and
// returns Abort on the first sustained budget violation. Stateless aside from its
// thresholds; identical input always yields an identical decision.
class SoakAbortPolicy {
  public:
    SoakAbortPolicy() = default;
    explicit SoakAbortPolicy(SoakThresholds thresholds) : t_(thresholds) {}

    [[nodiscard]] AbortDecision Evaluate(const std::vector<SoakSample>& history) const;

    [[nodiscard]] const SoakThresholds& thresholds() const {
        return t_;
    }

  private:
    SoakThresholds t_;
};

// Per-metric distribution over a run.
struct MetricStats {
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double p99 = 0.0;
    uint64_t count = 0; // samples that contributed (e.g. only available drift samples)
};

// End-of-run rollup produced from the whole timeline.
struct SoakSummary {
    double duration_s = 0.0;
    uint64_t sample_count = 0;

    MetricStats av_drift_ms;       // over available samples only
    MetricStats duration_skew_ms;  // over available samples only
    MetricStats mux_queue_depth;
    MetricStats disk_fill_eta_s;   // over available (>=0) samples only
    MetricStats rss_bytes;
    MetricStats private_bytes;
    MetricStats handle_count;
    MetricStats gdi_objects;
    MetricStats user_objects;

    // Least-squares slope of value vs t_s over the whole run.
    double rss_slope_bytes_per_s = 0.0;
    double private_slope_bytes_per_s = 0.0;
    double handle_slope_per_s = 0.0;

    // Cumulative totals taken from the last sample.
    uint64_t frames_captured = 0;
    uint64_t frames_emitted = 0;
    uint64_t total_frames_dropped = 0;
    uint64_t total_frames_duplicated = 0;
    uint64_t total_audio_discontinuities = 0;

    // The abort verdict the policy would render on the full history (advisory).
    AbortDecision advisory_verdict;
};

// Pure aggregator. No I/O.
class SoakMetricsAggregator {
  public:
    SoakMetricsAggregator() = default;
    explicit SoakMetricsAggregator(SoakThresholds thresholds) : t_(thresholds) {}

    [[nodiscard]] SoakSummary Summarize(const std::vector<SoakSample>& history) const;

  private:
    SoakThresholds t_;
};

// --- Pure serialization helpers (string in / string out; no file I/O) ---

// One JSON object per sample, newline-terminated — appended to the .jsonl timeline.
[[nodiscard]] std::string SampleToJsonLine(const SoakSample& s);

// The end-of-run report as a pretty JSON document. `metadata` key/value pairs
// (volume, container/codec, ffprobe result, durability_flush_count, ...) are
// emitted verbatim under "metadata".
[[nodiscard]] std::string SummaryToJson(const SoakSummary& summary,
                                        const std::vector<std::pair<std::string, std::string>>& metadata);

// A short human-readable Markdown digest for the console / report sidecar.
[[nodiscard]] std::string SummaryToMarkdown(const SoakSummary& summary,
                                            const std::vector<std::pair<std::string, std::string>>& metadata);

} // namespace exosnap::soak
