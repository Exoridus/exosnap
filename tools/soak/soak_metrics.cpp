#include "soak_metrics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace exosnap::soak {

namespace {

// Least-squares slope of y over x. Returns 0 for <2 points or degenerate x.
double LinearSlope(const std::vector<double>& xs, const std::vector<double>& ys) {
    const size_t n = xs.size();
    if (n < 2 || ys.size() != n)
        return 0.0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
    }
    const double denom = static_cast<double>(n) * sxx - sx * sx;
    if (std::abs(denom) < 1e-9)
        return 0.0;
    return (static_cast<double>(n) * sxy - sx * sy) / denom;
}

MetricStats StatsOf(std::vector<double> values) {
    MetricStats st;
    st.count = values.size();
    if (values.empty())
        return st;
    std::sort(values.begin(), values.end());
    st.min = values.front();
    st.max = values.back();
    double sum = 0;
    for (double v : values)
        sum += v;
    st.mean = sum / static_cast<double>(values.size());
    // p99 by nearest-rank on the sorted vector.
    const size_t idx =
        static_cast<size_t>(std::ceil(0.99 * static_cast<double>(values.size())) - 1.0);
    st.p99 = values[std::min(idx, values.size() - 1)];
    return st;
}

// True when the trailing `window` samples all satisfy `pred`.
template <typename Pred>
bool SustainedTail(const std::vector<SoakSample>& h, int window, Pred pred) {
    const int n = static_cast<int>(h.size());
    if (n < window || window <= 0)
        return false;
    for (int i = n - window; i < n; ++i) {
        if (!pred(h[static_cast<size_t>(i)]))
            return false;
    }
    return true;
}

double DropRatio(const SoakSample& s) {
    const uint64_t emitted = std::max<uint64_t>(1, s.frames_emitted);
    return static_cast<double>(s.total_dropped()) / static_cast<double>(emitted);
}

} // namespace

AbortDecision SoakAbortPolicy::Evaluate(const std::vector<SoakSample>& h) const {
    if (h.empty())
        return {};

    const SoakSample& last = h.back();

    // 1. A hard engine failure ends the run immediately.
    if (last.recorder_failed)
        return {SoakVerdict::Abort, "recorder session reported a failure"};

    // 2. Sustained Critical health.
    if (SustainedTail(h, t_.sustained_samples, [](const SoakSample& s) { return s.health_critical; }))
        return {SoakVerdict::Abort, "pipeline health Critical was sustained"};

    // 3. Duration skew over budget AND monotonically growing across the window.
    if (SustainedTail(h, t_.sustained_samples, [&](const SoakSample& s) {
            return s.duration_skew_available && std::abs(s.duration_skew_ms) > t_.duration_skew_abort_ms;
        })) {
        const int n = static_cast<int>(h.size());
        bool growing = true;
        for (int i = n - t_.sustained_samples + 1; i < n; ++i) {
            if (std::abs(h[static_cast<size_t>(i)].duration_skew_ms) <
                std::abs(h[static_cast<size_t>(i - 1)].duration_skew_ms)) {
                growing = false;
                break;
            }
        }
        if (growing)
            return {SoakVerdict::Abort, "duration skew over budget and still growing"};
    }

    // 4. A/V clock drift over budget (available samples), sustained.
    if (SustainedTail(h, t_.sustained_samples, [&](const SoakSample& s) {
            return s.av_drift_available && std::abs(s.av_drift_ms) > t_.av_drift_abort_ms;
        }))
        return {SoakVerdict::Abort, "A/V clock drift over budget"};

    // 5. Drop ratio over budget, sustained.
    if (SustainedTail(h, t_.sustained_samples,
                      [&](const SoakSample& s) { return DropRatio(s) > t_.drop_ratio_abort; }))
        return {SoakVerdict::Abort, "frame drop ratio over budget"};

    // 6. Leak slope over threshold (only once a baseline window exists).
    if (static_cast<int>(h.size()) >= t_.min_slope_samples) {
        std::vector<double> xs, rss, hnd;
        xs.reserve(h.size());
        rss.reserve(h.size());
        hnd.reserve(h.size());
        for (const auto& s : h) {
            xs.push_back(s.t_s);
            rss.push_back(static_cast<double>(s.rss_bytes));
            hnd.push_back(static_cast<double>(s.handle_count));
        }
        if (LinearSlope(xs, rss) > t_.rss_slope_abort_bytes_per_s)
            return {SoakVerdict::Abort, "working-set leak slope over threshold"};
        if (LinearSlope(xs, hnd) > t_.handle_slope_abort_per_s)
            return {SoakVerdict::Abort, "handle-count leak slope over threshold"};
    }

    return {};
}

SoakSummary SoakMetricsAggregator::Summarize(const std::vector<SoakSample>& h) const {
    SoakSummary out;
    out.sample_count = h.size();
    if (h.empty())
        return out;

    out.duration_s = h.back().t_s;

    std::vector<double> drift, skew, mqd, eta, rss, priv, hnd, gdi, usr;
    std::vector<double> xs, rss_x, priv_x, hnd_x;
    for (const auto& s : h) {
        if (s.av_drift_available)
            drift.push_back(s.av_drift_ms);
        if (s.duration_skew_available)
            skew.push_back(s.duration_skew_ms);
        mqd.push_back(static_cast<double>(s.mux_queue_depth));
        if (s.disk_fill_eta_s >= 0.0)
            eta.push_back(s.disk_fill_eta_s);
        rss.push_back(static_cast<double>(s.rss_bytes));
        priv.push_back(static_cast<double>(s.private_bytes));
        hnd.push_back(static_cast<double>(s.handle_count));
        gdi.push_back(static_cast<double>(s.gdi_objects));
        usr.push_back(static_cast<double>(s.user_objects));
        xs.push_back(s.t_s);
    }

    out.av_drift_ms = StatsOf(drift);
    out.duration_skew_ms = StatsOf(skew);
    out.mux_queue_depth = StatsOf(mqd);
    out.disk_fill_eta_s = StatsOf(eta);
    out.rss_bytes = StatsOf(rss);
    out.private_bytes = StatsOf(priv);
    out.handle_count = StatsOf(hnd);
    out.gdi_objects = StatsOf(gdi);
    out.user_objects = StatsOf(usr);

    out.rss_slope_bytes_per_s = LinearSlope(xs, rss);
    out.private_slope_bytes_per_s = LinearSlope(xs, priv);
    out.handle_slope_per_s = LinearSlope(xs, hnd);

    const SoakSample& last = h.back();
    out.frames_captured = last.frames_captured;
    out.frames_emitted = last.frames_emitted;
    out.total_frames_dropped = last.total_dropped();
    out.total_frames_duplicated = last.frames_duplicated;
    out.total_audio_discontinuities = last.audio_discontinuities;

    out.advisory_verdict = SoakAbortPolicy(t_).Evaluate(h);
    return out;
}

// --- Serialization ---

std::string SampleToJsonLine(const SoakSample& s) {
    nlohmann::json j;
    j["t_s"] = s.t_s;
    j["av_drift_ms"] = s.av_drift_ms;
    j["av_drift_available"] = s.av_drift_available;
    j["duration_skew_ms"] = s.duration_skew_ms;
    j["duration_skew_available"] = s.duration_skew_available;
    j["frames_captured"] = s.frames_captured;
    j["frames_emitted"] = s.frames_emitted;
    j["frames_dropped_coalesced"] = s.frames_dropped_coalesced;
    j["frames_dropped_cfr"] = s.frames_dropped_cfr;
    j["frames_dropped_backpressure"] = s.frames_dropped_backpressure;
    j["frames_dropped_processing_failure"] = s.frames_dropped_processing_failure;
    j["frames_duplicated"] = s.frames_duplicated;
    j["audio_discontinuities"] = s.audio_discontinuities;
    j["mux_queue_depth"] = s.mux_queue_depth;
    j["disk_fill_eta_s"] = s.disk_fill_eta_s;
    j["rss_bytes"] = s.rss_bytes;
    j["private_bytes"] = s.private_bytes;
    j["handle_count"] = s.handle_count;
    j["gdi_objects"] = s.gdi_objects;
    j["user_objects"] = s.user_objects;
    j["health_critical"] = s.health_critical;
    j["bottleneck"] = s.bottleneck;
    return j.dump() + "\n";
}

namespace {
nlohmann::json StatsJson(const MetricStats& m) {
    return nlohmann::json{{"min", m.min}, {"max", m.max}, {"mean", m.mean}, {"p99", m.p99}, {"count", m.count}};
}
} // namespace

std::string SummaryToJson(const SoakSummary& s,
                          const std::vector<std::pair<std::string, std::string>>& metadata) {
    nlohmann::json j;
    j["duration_s"] = s.duration_s;
    j["sample_count"] = s.sample_count;
    j["metrics"] = {
        {"av_drift_ms", StatsJson(s.av_drift_ms)},
        {"duration_skew_ms", StatsJson(s.duration_skew_ms)},
        {"mux_queue_depth", StatsJson(s.mux_queue_depth)},
        {"disk_fill_eta_s", StatsJson(s.disk_fill_eta_s)},
        {"rss_bytes", StatsJson(s.rss_bytes)},
        {"private_bytes", StatsJson(s.private_bytes)},
        {"handle_count", StatsJson(s.handle_count)},
        {"gdi_objects", StatsJson(s.gdi_objects)},
        {"user_objects", StatsJson(s.user_objects)},
    };
    j["leak_slopes"] = {
        {"rss_bytes_per_s", s.rss_slope_bytes_per_s},
        {"private_bytes_per_s", s.private_slope_bytes_per_s},
        {"handle_per_s", s.handle_slope_per_s},
    };
    j["totals"] = {
        {"frames_captured", s.frames_captured},
        {"frames_emitted", s.frames_emitted},
        {"frames_dropped", s.total_frames_dropped},
        {"frames_duplicated", s.total_frames_duplicated},
        {"audio_discontinuities", s.total_audio_discontinuities},
    };
    j["advisory_verdict"] = {
        {"aborted", s.advisory_verdict.verdict == SoakVerdict::Abort},
        {"reason", s.advisory_verdict.reason},
    };
    j["advisory_note"] =
        "Thresholds are advisory for 0.10; a crossing is not a release gate. See "
        "docs/dev/soak-and-recovery-drills.md.";
    nlohmann::json meta = nlohmann::json::object();
    for (const auto& [k, v] : metadata)
        meta[k] = v;
    j["metadata"] = meta;
    return j.dump(2) + "\n";
}

std::string SummaryToMarkdown(const SoakSummary& s,
                              const std::vector<std::pair<std::string, std::string>>& metadata) {
    std::string md;
    md += "# Soak report\n\n";
    md += "- duration: " + std::to_string(s.duration_s) + " s\n";
    md += "- samples: " + std::to_string(s.sample_count) + "\n";
    for (const auto& [k, v] : metadata)
        md += "- " + k + ": " + v + "\n";
    md += "\n## Metrics (advisory budgets)\n\n";
    auto line = [&](const char* name, const MetricStats& m) {
        md += "- " + std::string(name) + ": min=" + std::to_string(m.min) + " mean=" + std::to_string(m.mean) +
              " p99=" + std::to_string(m.p99) + " max=" + std::to_string(m.max) + " (n=" + std::to_string(m.count) +
              ")\n";
    };
    line("av_drift_ms", s.av_drift_ms);
    line("duration_skew_ms", s.duration_skew_ms);
    line("mux_queue_depth", s.mux_queue_depth);
    line("rss_bytes", s.rss_bytes);
    line("handle_count", s.handle_count);
    md += "\n## Leak slopes (advisory)\n\n";
    md += "- rss: " + std::to_string(s.rss_slope_bytes_per_s) + " bytes/s\n";
    md += "- private: " + std::to_string(s.private_slope_bytes_per_s) + " bytes/s\n";
    md += "- handles: " + std::to_string(s.handle_slope_per_s) + " /s\n";
    md += "\n## Totals\n\n";
    md += "- frames captured: " + std::to_string(s.frames_captured) + "\n";
    md += "- frames emitted: " + std::to_string(s.frames_emitted) + "\n";
    md += "- frames dropped: " + std::to_string(s.total_frames_dropped) + "\n";
    md += "- frames duplicated: " + std::to_string(s.total_frames_duplicated) + "\n";
    md += "- audio discontinuities: " + std::to_string(s.total_audio_discontinuities) + "\n";
    md += "\n**Verdict (advisory):** ";
    md += (s.advisory_verdict.verdict == SoakVerdict::Abort) ? ("ABORT — " + s.advisory_verdict.reason) : "no abort";
    md += "\n";
    return md;
}

} // namespace exosnap::soak
