// test_soak_policy.cpp — the whole soak decision surface, exercised without a GPU,
// a real file, or any WinAPI. Synthetic timelines in, verdict / summary out.

#include "soak_metrics.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>

using exosnap::soak::SampleToJsonLine;
using exosnap::soak::SoakAbortPolicy;
using exosnap::soak::SoakMetricsAggregator;
using exosnap::soak::SoakSample;
using exosnap::soak::SoakThresholds;
using exosnap::soak::SoakVerdict;
using exosnap::soak::SummaryToJson;

namespace {

// A healthy 1 Hz timeline: flat RAM/handles, no drift, no drops.
std::vector<SoakSample> HealthyTimeline(int seconds) {
    std::vector<SoakSample> h;
    for (int i = 0; i < seconds; ++i) {
        SoakSample s;
        s.t_s = static_cast<double>(i);
        s.frames_emitted = static_cast<uint64_t>(i) * 60;
        s.frames_captured = static_cast<uint64_t>(i) * 60;
        s.duration_skew_available = true;
        s.duration_skew_ms = 0.5; // well within budget
        s.av_drift_available = true;
        s.av_drift_ms = 1.0;
        s.rss_bytes = 400ULL * 1024 * 1024; // steady 400 MiB
        s.private_bytes = 420ULL * 1024 * 1024;
        s.handle_count = 500;
        h.push_back(s);
    }
    return h;
}

} // namespace

TEST(SoakAbortPolicy, HealthyRunNeverAborts) {
    const auto h = HealthyTimeline(600);
    const auto d = SoakAbortPolicy{}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Continue) << d.reason;
}

TEST(SoakAbortPolicy, EmptyHistoryContinues) {
    EXPECT_EQ(SoakAbortPolicy{}.Evaluate({}).verdict, SoakVerdict::Continue);
}

TEST(SoakAbortPolicy, RecorderFailureAbortsImmediately) {
    auto h = HealthyTimeline(5);
    h.back().recorder_failed = true;
    const auto d = SoakAbortPolicy{}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Abort);
    EXPECT_NE(d.reason.find("failure"), std::string::npos);
}

TEST(SoakAbortPolicy, SkewRampOverBudgetAndGrowingAborts) {
    SoakThresholds t;
    t.sustained_samples = 10;
    auto h = HealthyTimeline(200);
    // Ramp skew up past budget across the tail, strictly growing.
    for (size_t i = h.size() - 40; i < h.size(); ++i) {
        h[i].duration_skew_ms = 21.0 + static_cast<double>(i - (h.size() - 40)) * 1.0;
    }
    const auto d = SoakAbortPolicy{t}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Abort);
    EXPECT_NE(d.reason.find("skew"), std::string::npos);
}

TEST(SoakAbortPolicy, SingleSkewSpikeDoesNotAbort) {
    SoakThresholds t;
    t.sustained_samples = 10;
    auto h = HealthyTimeline(200);
    h[150].duration_skew_ms = 500.0; // one transient spike
    const auto d = SoakAbortPolicy{t}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Continue) << d.reason;
}

TEST(SoakAbortPolicy, DriftIgnoredWhenUnavailable) {
    SoakThresholds t;
    t.sustained_samples = 10;
    auto h = HealthyTimeline(200);
    for (auto& s : h) {
        s.av_drift_ms = 999.0;        // huge, but...
        s.av_drift_available = false; // ...never reported
    }
    EXPECT_EQ(SoakAbortPolicy{t}.Evaluate(h).verdict, SoakVerdict::Continue);
}

TEST(SoakAbortPolicy, SustainedDriftOverBudgetAborts) {
    SoakThresholds t;
    t.sustained_samples = 10;
    auto h = HealthyTimeline(200);
    for (size_t i = h.size() - 20; i < h.size(); ++i)
        h[i].av_drift_ms = 30.0; // over the 20 ms budget, available
    const auto d = SoakAbortPolicy{t}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Abort);
    EXPECT_NE(d.reason.find("drift"), std::string::npos);
}

TEST(SoakAbortPolicy, InjectedRssLeakSlopeAborts) {
    SoakThresholds t;
    t.min_slope_samples = 60;
    auto h = HealthyTimeline(300);
    // 1 MiB/s climb — far over the 256 KiB/s slope threshold.
    for (size_t i = 0; i < h.size(); ++i)
        h[i].rss_bytes = 400ULL * 1024 * 1024 + static_cast<uint64_t>(i) * 1024 * 1024;
    const auto d = SoakAbortPolicy{t}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Abort);
    EXPECT_NE(d.reason.find("leak"), std::string::npos);
}

TEST(SoakAbortPolicy, HandleLeakSlopeAborts) {
    SoakThresholds t;
    t.min_slope_samples = 60;
    auto h = HealthyTimeline(300);
    for (size_t i = 0; i < h.size(); ++i)
        h[i].handle_count = 500 + static_cast<uint32_t>(i * 3); // 3 handles/s
    const auto d = SoakAbortPolicy{t}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Abort);
    EXPECT_NE(d.reason.find("handle"), std::string::npos);
}

TEST(SoakAbortPolicy, LeakSlopeSuppressedBeforeBaselineWindow) {
    SoakThresholds t;
    t.min_slope_samples = 500; // longer than the run
    auto h = HealthyTimeline(100);
    for (size_t i = 0; i < h.size(); ++i)
        h[i].rss_bytes = 400ULL * 1024 * 1024 + static_cast<uint64_t>(i) * 10 * 1024 * 1024;
    EXPECT_EQ(SoakAbortPolicy{t}.Evaluate(h).verdict, SoakVerdict::Continue);
}

TEST(SoakAbortPolicy, SustainedCriticalHealthAborts) {
    SoakThresholds t;
    t.sustained_samples = 5;
    auto h = HealthyTimeline(50);
    for (size_t i = h.size() - 10; i < h.size(); ++i)
        h[i].health_critical = true;
    const auto d = SoakAbortPolicy{t}.Evaluate(h);
    EXPECT_EQ(d.verdict, SoakVerdict::Abort);
    EXPECT_NE(d.reason.find("Critical"), std::string::npos);
}

TEST(SoakMetricsAggregator, SummarizesHealthyRun) {
    const auto h = HealthyTimeline(600);
    const auto s = SoakMetricsAggregator{}.Summarize(h);
    EXPECT_EQ(s.sample_count, 600u);
    EXPECT_NEAR(s.duration_s, 599.0, 1e-6);
    EXPECT_NEAR(s.rss_slope_bytes_per_s, 0.0, 1.0);
    EXPECT_NEAR(s.handle_slope_per_s, 0.0, 1e-6);
    EXPECT_EQ(s.av_drift_ms.count, 600u);
    EXPECT_NEAR(s.av_drift_ms.mean, 1.0, 1e-6);
    EXPECT_EQ(s.advisory_verdict.verdict, SoakVerdict::Continue);
}

TEST(SoakMetricsAggregator, ReportsLeakSlope) {
    auto h = HealthyTimeline(100);
    for (size_t i = 0; i < h.size(); ++i)
        h[i].rss_bytes = 400ULL * 1024 * 1024 + static_cast<uint64_t>(i) * 1024 * 1024; // 1 MiB/s
    const auto s = SoakMetricsAggregator{}.Summarize(h);
    EXPECT_NEAR(s.rss_slope_bytes_per_s, 1024.0 * 1024.0, 1024.0);
}

TEST(SoakMetricsAggregator, DriftStatsOnlyCountAvailableSamples) {
    auto h = HealthyTimeline(100);
    for (size_t i = 0; i < 50; ++i)
        h[i].av_drift_available = false;
    const auto s = SoakMetricsAggregator{}.Summarize(h);
    EXPECT_EQ(s.av_drift_ms.count, 50u);
}

TEST(Serialization, SampleLineIsWellFormedAndNewlineTerminated) {
    SoakSample s;
    s.t_s = 3.5;
    s.rss_bytes = 12345;
    s.duration_skew_available = true;
    const std::string line = SampleToJsonLine(s);
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(line.back(), '\n');
    EXPECT_NE(line.find("\"t_s\""), std::string::npos);
    EXPECT_NE(line.find("\"rss_bytes\""), std::string::npos);
}

TEST(Serialization, SummaryJsonCarriesMetadataAndAdvisoryNote) {
    const auto h = HealthyTimeline(30);
    const auto s = SoakMetricsAggregator{}.Summarize(h);
    const std::string j = SummaryToJson(s, {{"volume", "C:"}, {"container", "mkv"}});
    EXPECT_NE(j.find("\"metadata\""), std::string::npos);
    EXPECT_NE(j.find("\"volume\""), std::string::npos);
    EXPECT_NE(j.find("advisory"), std::string::npos);
}

TEST(SoakAbortPolicy, HighCoalescingDoesNotAbort) {
    auto history = HealthyTimeline(90);
    for (auto& sample : history)
        sample.frames_dropped_coalesced = sample.frames_emitted * 20 + 1000;
    const auto decision = SoakAbortPolicy{}.Evaluate(history);
    EXPECT_EQ(decision.verdict, SoakVerdict::Continue) << decision.reason;
}

TEST(SoakAbortPolicy, CfrWarmupDoesNotAbort) {
    auto history = HealthyTimeline(90);
    for (auto& sample : history)
        sample.frames_dropped_cfr = 1000;
    const auto decision = SoakAbortPolicy{}.Evaluate(history);
    EXPECT_EQ(decision.verdict, SoakVerdict::Continue) << decision.reason;
}

TEST(SoakAbortPolicy, SustainedBackpressureAndProcessingLossStillAbort) {
    for (const bool processing : {false, true}) {
        auto history = HealthyTimeline(90);
        for (auto& sample : history) {
            sample.real_frame_loss = sample.frames_emitted / 10 + 1;
            if (processing)
                sample.frames_dropped_processing_failure = sample.real_frame_loss;
            else
                sample.frames_dropped_backpressure = sample.real_frame_loss;
        }
        EXPECT_EQ(SoakAbortPolicy{}.Evaluate(history).verdict, SoakVerdict::Abort) << processing;
    }
}

TEST(SoakAbortPolicy, RingEvictionsRemainEventsWithoutBecomingOutputLoss) {
    auto history = HealthyTimeline(90);
    for (auto& sample : history)
        sample.frames_dropped_ring_eviction = 100000;
    EXPECT_EQ(SoakAbortPolicy{}.Evaluate(history).verdict, SoakVerdict::Continue);
    EXPECT_EQ(SoakMetricsAggregator{}.Summarize(history).frames_dropped_ring_eviction, 100000u);
}

TEST(SoakAbortPolicy, MixedEventsUseOnlyCanonicalLossAndKeepTheTwoPercentBudget) {
    auto history = HealthyTimeline(90);
    for (auto& sample : history) {
        sample.frames_dropped_coalesced = 100000;
        sample.frames_dropped_cfr = 20000;
        sample.frames_dropped_ring_eviction = 50000;
        sample.frames_dropped_backpressure = sample.frames_emitted / 100;
        sample.real_frame_loss = sample.frames_dropped_backpressure;
    }
    EXPECT_EQ(SoakAbortPolicy{}.Evaluate(history).verdict, SoakVerdict::Continue);
    for (auto& sample : history) {
        sample.frames_dropped_processing_failure = sample.frames_emitted / 20 + 1;
        sample.real_frame_loss += sample.frames_dropped_processing_failure;
    }
    EXPECT_EQ(SoakAbortPolicy{}.Evaluate(history).verdict, SoakVerdict::Abort);
    EXPECT_DOUBLE_EQ(SoakAbortPolicy{}.thresholds().drop_ratio_abort, 0.02);
}

TEST(SoakAbortPolicy, TerminalUndrainedEncoderOutputCannotPassAShortRun) {
    auto history = HealthyTimeline(2);
    history.back().encoder_undrained_frames = 1;
    history.back().real_frame_loss = 1;
    EXPECT_EQ(SoakAbortPolicy{}.Evaluate(history).verdict, SoakVerdict::Abort);
    history.back().encoder_undrained_frames = 0;
    history.back().encoder_flush_incomplete = true;
    EXPECT_EQ(SoakAbortPolicy{}.Evaluate(history).verdict, SoakVerdict::Abort);
}

TEST(Serialization, ReportsAllEventsSeparatelyFromCanonicalPictureLoss) {
    SoakSample sample;
    sample.frames_dropped_coalesced = 1000;
    sample.frames_dropped_cfr = 2000;
    sample.frames_dropped_backpressure = 30;
    sample.frames_dropped_processing_failure = 40;
    sample.frames_dropped_ring_eviction = 50;
    sample.encoder_undrained_frames = 60;
    sample.real_frame_loss = 130;
    const auto summary = SoakMetricsAggregator{}.Summarize({sample});
    EXPECT_EQ(summary.total_frames_dropped, 3180u);
    EXPECT_EQ(summary.real_frame_loss, 130u);
    const auto json = nlohmann::json::parse(SummaryToJson(summary, {}));
    EXPECT_EQ(json["totals"]["frames_dropped"], 3180);
    EXPECT_EQ(json["totals"]["real_frame_loss"], 130);
    EXPECT_EQ(json["totals"]["frames_dropped_ring_eviction"], 50);
    EXPECT_EQ(json["totals"]["encoder_undrained_frames"], 60);
    const auto row = nlohmann::json::parse(SampleToJsonLine(sample));
    EXPECT_EQ(row["real_frame_loss"], 130);
    EXPECT_EQ(row["frames_dropped_ring_eviction"], 50);
    const auto markdown = exosnap::soak::SummaryToMarkdown(summary, {});
    EXPECT_NE(markdown.find("drop events (all categories): 3180"), std::string::npos);
    EXPECT_NE(markdown.find("real frame loss: 130"), std::string::npos);
    EXPECT_NE(markdown.find("ring eviction: 50"), std::string::npos);
}
