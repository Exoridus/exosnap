#include <gtest/gtest.h>

#include "pipeline_diagnostics_aggregator.h"

#include <chrono>

namespace {

using namespace recorder_core;
using clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Fixed, deterministic time base well away from the epoch (no negative durations).
const clock::time_point kBase = clock::time_point{} + std::chrono::hours(10);
clock::time_point At(int64_t ms) {
    return kBase + milliseconds(ms);
}

DiagnosticsStaticConfig MakeConfig(bool split_supported = true, bool audio = true) {
    DiagnosticsStaticConfig cfg;
    cfg.source_type = CaptureSourceType::Display;
    cfg.split_supported = split_supported;
    cfg.auto_split = false;
    cfg.auto_split_seconds = 0.0;
    cfg.output_target = "C:";
    cfg.audio_present = audio;
    cfg.audio_track_count = audio ? 1u : 0u;
    cfg.video_queue_capacity = 0;
    cfg.audio_queue_capacity = 600;
    return cfg;
}

SessionStats MakeStats() {
    SessionStats s;
    s.frame_rate_num = 60;
    s.frame_rate_den = 1;
    s.cfr = true;
    s.video_codec = VideoCodec::Av1Nvenc;
    s.audio_codec = AudioCodec::Opus;
    s.output_size = FrameSize{1920, 1080};
    return s;
}

// ---------------------------------------------------------------------------
// RollingTimeWindow
// ---------------------------------------------------------------------------

TEST(RollingTimeWindow, EmptyComputeIsZero) {
    RollingTimeWindow w(64, milliseconds(2000));
    const auto a = w.Compute(At(0));
    EXPECT_EQ(a.count, 0u);
    EXPECT_DOUBLE_EQ(a.latest, 0.0);
    EXPECT_DOUBLE_EQ(a.average, 0.0);
    EXPECT_DOUBLE_EQ(a.peak, 0.0);
}

TEST(RollingTimeWindow, LatestAveragePeak) {
    RollingTimeWindow w(64, milliseconds(2000));
    w.Add(At(0), 2.0);
    w.Add(At(10), 6.0);
    w.Add(At(20), 4.0);
    const auto a = w.Compute(At(30));
    EXPECT_EQ(a.count, 3u);
    EXPECT_DOUBLE_EQ(a.latest, 4.0);
    EXPECT_DOUBLE_EQ(a.peak, 6.0);
    EXPECT_DOUBLE_EQ(a.average, 4.0);
}

TEST(RollingTimeWindow, PeakExpiresWithHorizon) {
    RollingTimeWindow w(64, milliseconds(2000));
    w.Add(At(0), 10.0);
    w.Add(At(1500), 2.0);
    EXPECT_DOUBLE_EQ(w.Compute(At(1600)).peak, 10.0); // both in horizon
    EXPECT_DOUBLE_EQ(w.Compute(At(2200)).peak, 2.0);  // At(0) expired (cutoff 200)
    EXPECT_EQ(w.Compute(At(2200)).count, 1u);
}

TEST(RollingTimeWindow, BoundedCapacityNoUnboundedGrowth) {
    RollingTimeWindow w(64, milliseconds(100000));
    for (int i = 0; i < 5000; ++i) {
        w.Add(At(i), static_cast<double>(i));
    }
    EXPECT_EQ(w.capacity(), 64u);
    EXPECT_EQ(w.size(), 64u); // never exceeds capacity
}

TEST(RollingTimeWindow, PercentileEmptyIsZero) {
    RollingTimeWindow w(64, milliseconds(2000));
    EXPECT_DOUBLE_EQ(w.Percentile(At(0), 0.5), 0.0);
}

TEST(RollingTimeWindow, PercentileExactNearestRank) {
    RollingTimeWindow w(256, milliseconds(100000));
    // Values 1..100 ms at 1 ms cadence, all inside the horizon.
    for (int i = 1; i <= 100; ++i) {
        w.Add(At(i), static_cast<double>(i));
    }
    // Nearest-rank on a 0-based sorted index of 100 elements.
    EXPECT_DOUBLE_EQ(w.Percentile(At(100), 0.0), 1.0);
    EXPECT_DOUBLE_EQ(w.Percentile(At(100), 1.0), 100.0);
    // p50 -> rank round(0.5*99)=round(49.5)=50 -> value 51.
    EXPECT_DOUBLE_EQ(w.Percentile(At(100), 0.5), 51.0);
    // p99 -> rank round(0.99*99)=round(98.01)=98 -> value 99.
    EXPECT_DOUBLE_EQ(w.Percentile(At(100), 0.99), 99.0);
}

TEST(RollingTimeWindow, PercentileHonoursHorizon) {
    RollingTimeWindow w(256, milliseconds(2000));
    // Old, large samples outside the 2 s horizon must not count.
    for (int i = 0; i < 10; ++i) {
        w.Add(At(i), 900.0);
    }
    // Fresh, small samples inside the horizon.
    for (int i = 0; i < 10; ++i) {
        w.Add(At(5000 + i), 3.0);
    }
    EXPECT_DOUBLE_EQ(w.Percentile(At(5009), 0.99), 3.0);
}

// ---------------------------------------------------------------------------
// Snapshot defaults / lifecycle / generation
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Audio device-loss health (ADR 0046)
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, AudioSourceHealthDefaultsHealthy) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.audio.degraded_sources, 0u);
    EXPECT_FALSE(s.audio.source_degraded);
}

TEST(PipelineDiagnostics, AudioSourceHealthSurfacesAndClearsDegradation) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());

    // One of a merged track's two sources lost its endpoint.
    agg.OnAudioSourceHealth(/*track*/ 0, /*degraded*/ 1, /*total*/ 2);
    auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.audio.degraded_sources, 1u);
    EXPECT_TRUE(s.audio.source_degraded);

    // It reactivated: the notice clears.
    agg.OnAudioSourceHealth(0, 0, 2);
    s = agg.BuildSnapshot(At(200), MakeStats(), DiagnosticsLifecycle::Recording, 0.2);
    EXPECT_EQ(s.audio.degraded_sources, 0u);
    EXPECT_FALSE(s.audio.source_degraded);
}

TEST(PipelineDiagnostics, AudioSourceHealthSumsAcrossTracks) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAudioSourceHealth(0, 1, 1);
    agg.OnAudioSourceHealth(1, 1, 2);
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.audio.degraded_sources, 2u);
    EXPECT_TRUE(s.audio.source_degraded);
}

TEST(PipelineDiagnostics, DefaultGenerationZeroAndIdleInvalid) {
    PipelineDiagnosticsAggregator agg;
    EXPECT_EQ(agg.generation(), 0u);
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Idle, 0.0);
    EXPECT_FALSE(s.valid);
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::None);
    EXPECT_EQ(s.health, PipelineHealth::Idle);
}

TEST(PipelineDiagnostics, ResetStampsGenerationAndClearsCounters) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(7, MakeConfig());
    EXPECT_EQ(agg.generation(), 7u);
    agg.OnFrameCaptured();
    agg.OnForcedKeyframe();

    agg.Reset(8, MakeConfig());
    EXPECT_EQ(agg.generation(), 8u);
    auto stats = MakeStats();
    const auto s = agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.session_generation, 8u);
    EXPECT_EQ(s.capture.frames_captured, 0u);
    EXPECT_EQ(s.video_encoder.forced_keyframes, 0u);
}

TEST(PipelineDiagnostics, RecordingSnapshotIsValidInitializingIsNot) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    EXPECT_FALSE(agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Initializing, 0.0).valid);
    EXPECT_TRUE(agg.BuildSnapshot(At(200), MakeStats(), DiagnosticsLifecycle::Recording, 0.2).valid);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, ActualFpsFromEmittedDelta) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();

    stats.video_frames_captured = 0;
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0); // baseline

    stats.video_frames_captured = 12; // +12 frames over 200 ms => 60 fps
    const auto s = agg.BuildSnapshot(At(200), stats, DiagnosticsLifecycle::Recording, 0.2);
    EXPECT_NEAR(s.capture.actual_fps, 60.0, 0.01);
    EXPECT_NEAR(s.capture.target_fps, 60.0, 0.01);
}

TEST(PipelineDiagnostics, DropCategoriesRemainDistinct) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnFrameDroppedCoalesced();
    agg.OnFrameDroppedCoalesced();
    agg.OnFrameDroppedCfr();
    agg.OnFrameDroppedBackpressure();
    agg.OnFrameDroppedBackpressure();
    agg.OnFrameDroppedBackpressure();
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.frames_dropped_coalesced, 2u);
    EXPECT_EQ(s.capture.frames_dropped_cfr, 1u);
    EXPECT_EQ(s.capture.frames_dropped_backpressure, 3u);
    EXPECT_EQ(s.capture.frames_dropped_total(), 6u);
}

TEST(PipelineDiagnostics, CfrFrameIntervalIsTargetDerivedAndMarkedUnavailable) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.interval_observed, MetricAvailability::Unavailable);
    EXPECT_NEAR(s.capture.frame_interval_ms, 1000.0 / 60.0, 0.01);
}

TEST(PipelineDiagnostics, ObservedIntervalMarkedAvailableOnVfr) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnObservedFrameInterval(At(0), 16.0);
    agg.OnObservedFrameInterval(At(16), 18.0);
    const auto s = agg.BuildSnapshot(At(20), MakeStats(), DiagnosticsLifecycle::Recording, 0.02);
    EXPECT_EQ(s.capture.interval_observed, MetricAvailability::Available);
    EXPECT_NEAR(s.capture.frame_interval_ms, 17.0, 0.01);
}

// ---------------------------------------------------------------------------
// Present cadence (VRR/CFR judder correlation, v0.8.0 / ADR 0033)
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, PresentCadenceUnavailableWithoutSamples) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.present_cadence_availability, MetricAvailability::Unavailable);
}

TEST(PipelineDiagnostics, PresentCadenceAvailableAndJitterDerived) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // >= 8 present intervals within the 2 s window: mostly ~8 ms (≈120 Hz) with one spike.
    for (int i = 0; i < 12; ++i) {
        const double iv = (i == 6) ? 20.0 : 8.0;
        agg.OnSourcePresentInterval(At(i * 8), iv, 1);
    }
    const auto s = agg.BuildSnapshot(At(120), MakeStats(), DiagnosticsLifecycle::Recording, 2.0);
    EXPECT_EQ(s.capture.present_cadence_availability, MetricAvailability::Available);
    // avg = (11*8 + 20)/12 = 9.0; peak = 20 → jitter = 11 ms (> 4 ms threshold).
    EXPECT_NEAR(s.capture.source_present_interval_ms, 9.0, 0.001);
    EXPECT_GT(s.capture.source_present_jitter_ms, 4.0);
    EXPECT_NEAR(s.capture.source_coalesce_ratio, 1.0, 0.001);
}

TEST(PipelineDiagnostics, PresentCadenceGatedByWarmup) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 12; ++i)
        agg.OnSourcePresentInterval(At(i * 8), 8.0, 2);
    // elapsed below warmup_seconds (default 1.0) → Unavailable despite enough samples.
    const auto s = agg.BuildSnapshot(At(96), MakeStats(), DiagnosticsLifecycle::Recording, 0.5);
    EXPECT_EQ(s.capture.present_cadence_availability, MetricAvailability::Unavailable);
}

TEST(PipelineDiagnostics, PresentCoalesceRatioAveraged) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 12; ++i)
        agg.OnSourcePresentInterval(At(i * 8), 8.0, 2); // 2 coalesced updates per acquire
    const auto s = agg.BuildSnapshot(At(120), MakeStats(), DiagnosticsLifecycle::Recording, 2.0);
    EXPECT_EQ(s.capture.present_cadence_availability, MetricAvailability::Available);
    EXPECT_NEAR(s.capture.source_coalesce_ratio, 2.0, 0.001);
}

TEST(PipelineDiagnostics, PresentCadenceResetClearsState) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 12; ++i)
        agg.OnSourcePresentInterval(At(i * 8), 8.0, 1);
    agg.Reset(2, MakeConfig());
    const auto s = agg.BuildSnapshot(At(120), MakeStats(), DiagnosticsLifecycle::Recording, 2.0);
    EXPECT_EQ(s.capture.present_cadence_availability, MetricAvailability::Unavailable);
}

TEST(PipelineDiagnostics, PauseDoesNotCreateFalseDrops) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    stats.video_frames_captured = 100;
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);

    // While paused: no new frames, no new drops.
    const auto s1 = agg.BuildSnapshot(At(200), stats, DiagnosticsLifecycle::Paused, 0.2);
    const auto s2 = agg.BuildSnapshot(At(400), stats, DiagnosticsLifecycle::Paused, 0.4);
    EXPECT_EQ(s1.capture.frames_dropped_total(), 0u);
    EXPECT_EQ(s2.capture.frames_dropped_total(), 0u);
    EXPECT_EQ(s2.bottleneck, PipelineBottleneck::None);
    EXPECT_NE(s2.health, PipelineHealth::Warning);
    EXPECT_NE(s2.health, PipelineHealth::Critical);
}

// ---------------------------------------------------------------------------
// Compositor (CPU submission timing)
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, CompositorTimingLatestAvgPeakAndActive) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnCompositorSubmit(At(0), 1.0, true);
    agg.OnCompositorSubmit(At(16), 3.0, true);
    agg.OnCompositorSubmit(At(32), 2.0, true);
    const auto s = agg.BuildSnapshot(At(40), MakeStats(), DiagnosticsLifecycle::Recording, 0.04);
    EXPECT_TRUE(s.compositor.active);
    EXPECT_DOUBLE_EQ(s.compositor.latest_ms, 2.0);
    EXPECT_DOUBLE_EQ(s.compositor.peak_ms, 3.0);
    EXPECT_DOUBLE_EQ(s.compositor.average_ms, 2.0);
    EXPECT_EQ(s.compositor.frames_composed, 3u);
}

TEST(PipelineDiagnostics, CompositorInactiveWhenPassSkipped) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnCompositorSubmit(At(0), 0.0, false); // no overlay -> no pass
    const auto s = agg.BuildSnapshot(At(10), MakeStats(), DiagnosticsLifecycle::Recording, 0.01);
    EXPECT_FALSE(s.compositor.active);
    EXPECT_EQ(s.compositor.frames_composed, 0u);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, EncoderSubmittedEncodedBacklogAndOutputFps) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();

    for (int i = 0; i < 12; ++i) {
        agg.OnEncodeSubmitted();
    }
    stats.encoded_video_packets = 0;
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0); // baseline

    stats.encoded_video_packets = 10; // +10 over 200 ms => 50 fps; backlog = 12 - 10 = 2
    const auto s = agg.BuildSnapshot(At(200), stats, DiagnosticsLifecycle::Recording, 0.2);
    EXPECT_EQ(s.video_encoder.frames_submitted, 12u);
    EXPECT_EQ(s.video_encoder.frames_encoded, 10u);
    EXPECT_EQ(s.video_encoder.backlog, 2u);
    EXPECT_NEAR(s.video_encoder.output_fps, 50.0, 0.01);
}

TEST(PipelineDiagnostics, EncodeLatencyAggregated) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnEncodeLatency(At(0), 2.0);
    agg.OnEncodeLatency(At(16), 4.0);
    const auto s = agg.BuildSnapshot(At(20), MakeStats(), DiagnosticsLifecycle::Recording, 0.02);
    EXPECT_DOUBLE_EQ(s.video_encoder.average_ms, 3.0);
    EXPECT_DOUBLE_EQ(s.video_encoder.peak_ms, 4.0);
    EXPECT_DOUBLE_EQ(s.video_encoder.latest_ms, 4.0);
}

TEST(PipelineDiagnostics, EncodePercentilesInSnapshot) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // 100 latency samples 1..100 ms, all inside the 2 s window.
    for (int i = 1; i <= 100; ++i) {
        agg.OnEncodeLatency(At(i), static_cast<double>(i));
    }
    const auto s = agg.BuildSnapshot(At(100), MakeStats(), DiagnosticsLifecycle::Recording, 0.1);
    // Nearest-rank window percentiles: p50 -> 51, p99 -> 99.
    EXPECT_DOUBLE_EQ(s.video_encoder.p50_ms, 51.0);
    EXPECT_DOUBLE_EQ(s.video_encoder.p99_ms, 99.0);
}

TEST(PipelineDiagnostics, VideoTickTimingSnapshotAndBudget) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats(); // 60 fps
    // Unavailable until a tick sample exists.
    const auto s0 = agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s0.video_timing.availability, MetricAvailability::Unavailable);
    EXPECT_NEAR(s0.video_timing.budget_ms, 1000.0 / 60.0, 1e-6);

    for (int i = 1; i <= 50; ++i) {
        agg.OnVideoTickTime(At(1000 + i), static_cast<double>(i) / 10.0); // 0.1..5.0 ms
    }
    const auto s1 = agg.BuildSnapshot(At(1050), stats, DiagnosticsLifecycle::Recording, 1.05);
    EXPECT_EQ(s1.video_timing.availability, MetricAvailability::Available);
    EXPECT_GT(s1.video_timing.tick_p99_ms, s1.video_timing.tick_p50_ms);
    EXPECT_NEAR(s1.video_timing.tick_peak_ms, 5.0, 1e-9);
}

TEST(PipelineDiagnostics, SlotStallCounterIndependentOfBackpressure) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnFrameDroppedBackpressure();
    agg.OnFrameDroppedBackpressure();
    agg.OnSlotStall(); // a subset — only one of the two drops was a slot stall
    const auto p = agg.SamplePerfWindow(At(0));
    EXPECT_EQ(p.dropped_backpressure, 2u);
    EXPECT_EQ(p.slot_stalls, 1u);
}

TEST(PipelineDiagnostics, PerfWindowSampleSeparatesLatencyFromSubmitCost) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 20; ++i) {
        agg.OnEncodeLatency(At(i), 8.0);    // true latency
        agg.OnEncodeSubmitCost(At(i), 0.5); // cheap submit
        agg.OnVideoTickTime(At(i), 12.0);
    }
    const auto p = agg.SamplePerfWindow(At(20));
    EXPECT_NEAR(p.encode_latency.p50_ms, 8.0, 1e-9);
    EXPECT_NEAR(p.encode_submit.p50_ms, 0.5, 1e-9);
    EXPECT_NEAR(p.tick.p50_ms, 12.0, 1e-9);
    EXPECT_EQ(p.encode_latency.samples, 20u);
    EXPECT_EQ(p.tick.samples, 20u);
}

TEST(PipelineDiagnostics, PerfSessionSummaryHistogramsAndReset) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 30; ++i) {
        agg.OnEncodeLatency(At(i), 5.0);
        agg.OnVideoTickTime(At(i), 10.0);
        agg.OnEncodeSubmitCost(At(i), 1.0);
    }
    const auto sum = agg.BuildPerfSummary();
    EXPECT_EQ(sum.encode_latency.count, 30u);
    EXPECT_EQ(sum.tick.count, 30u);
    EXPECT_EQ(sum.encode_submit.count, 30u);
    // The whole-session encode p50 must land in the 5 ms bucket range.
    const std::size_t b5 = recorder_core::LatencyHistogram::BucketIndex(5.0);
    EXPECT_EQ(sum.encode_latency.buckets[b5], 30u);
    EXPECT_GE(sum.encode_latency.p50_ms, recorder_core::LatencyHistogram::BucketLowEdge(b5));
    EXPECT_LE(sum.encode_latency.p50_ms, recorder_core::LatencyHistogram::BucketHighEdge(b5));

    // Reset clears the whole-session histograms too.
    agg.Reset(2, MakeConfig());
    const auto empty = agg.BuildPerfSummary();
    EXPECT_EQ(empty.encode_latency.count, 0u);
    EXPECT_EQ(empty.tick.count, 0u);
}

TEST(PipelineDiagnostics, ForcedKeyframeCounterIncrementsOnSplitArm) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnForcedKeyframe();
    agg.OnForcedKeyframe();
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.video_encoder.forced_keyframes, 2u);
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, AudioFormatQueuePeakAndDiscontinuities) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.SetAudioFormat(48000, 2);
    agg.OnAudioQueueDepth(3);
    agg.OnAudioQueueDepth(7);
    agg.OnAudioQueueDepth(4);
    agg.OnAudioDiscontinuity();
    auto stats = MakeStats();
    stats.audio_packets = 50;
    stats.audio_bytes = 2048;
    const auto s = agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.audio.sample_rate, 48000u);
    EXPECT_EQ(s.audio.channels, 2u);
    EXPECT_EQ(s.audio.queue_depth, 4u);
    EXPECT_EQ(s.audio.queue_peak, 7u);
    EXPECT_EQ(s.audio.discontinuities, 1u);
    EXPECT_EQ(s.audio.packets_encoded, 50u);
    EXPECT_EQ(s.audio.bytes_encoded, 2048u);
}

// ---------------------------------------------------------------------------
// Queues
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, QueueCapacityVideoUnboundedAudioBounded) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnVideoQueueDepth(2);
    agg.OnVideoQueueDepth(5);
    agg.OnAudioPremuxDepth(10);
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.video_queue.current_depth, 5u);
    EXPECT_EQ(s.video_queue.peak_depth, 5u);
    EXPECT_FALSE(s.video_queue.bounded);
    EXPECT_EQ(s.video_queue.capacity, 0u);
    EXPECT_TRUE(s.audio_queue.bounded);
    EXPECT_EQ(s.audio_queue.capacity, 600u);
    EXPECT_EQ(s.audio_queue.peak_depth, 10u);
}

// ---------------------------------------------------------------------------
// Mux / Disk
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, MuxBytesThroughputAndWriteLatency) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    agg.OnDiskWrite(At(0), 0.5, 0);
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0); // baseline

    agg.OnMuxPacket(1000);
    agg.OnDiskWrite(At(100), 1.5, 2u * 1024u * 1024u); // 2 MiB written over the interval
    const auto s = agg.BuildSnapshot(At(200), stats, DiagnosticsLifecycle::Recording, 0.2);
    EXPECT_EQ(s.mux.packets_processed, 1u);
    EXPECT_EQ(s.mux.bytes_written, 2u * 1024u * 1024u);
    EXPECT_NEAR(s.mux.throughput_mib_s, 10.0, 0.01); // 2 MiB / 0.2 s
    EXPECT_NEAR(s.disk.throughput_mib_s, 10.0, 0.01);
    EXPECT_DOUBLE_EQ(s.disk.latest_write_ms, 1.5);
    EXPECT_EQ(s.disk.output_target, "C:");
}

TEST(PipelineDiagnostics, SegmentCountersAndFinalizeDuration) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig(/*split_supported=*/true));
    agg.OnSegmentOpened(0);
    agg.OnSegmentFinalized(3.5, true); // finalize segment 0
    agg.OnSplitTransition(DiagnosticsSplitTrigger::ManualButton);
    agg.OnSegmentOpened(1);
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.mux.current_segment_index, 1u);
    EXPECT_EQ(s.mux.segment_count, 2u);
    EXPECT_EQ(s.mux.finalizations, 1u);
    EXPECT_DOUBLE_EQ(s.mux.latest_finalize_ms, 3.5);
    EXPECT_EQ(s.split.current_segment, 2u); // 1-based
    EXPECT_EQ(s.split.completed_segments, 1u);
    EXPECT_EQ(s.split.last_trigger, DiagnosticsSplitTrigger::ManualButton);
    EXPECT_TRUE(s.split.split_supported);
}

TEST(PipelineDiagnostics, ReorderWindowPeaksBoundedAndMonotone) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnReorderWindow(3, 3, 3000, 3000);
    agg.OnReorderWindow(1, 3, 1000, 3000); // current drops, peak holds
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.mux.reorder_packets, 1u);
    EXPECT_EQ(s.mux.reorder_packets_peak, 3u);
    EXPECT_EQ(s.mux.reorder_bytes, 1000u);
    EXPECT_EQ(s.mux.reorder_bytes_peak, 3000u);
    EXPECT_EQ(s.mux.availability, MetricAvailability::Available);
}

TEST(PipelineDiagnostics, Mp4SplitAndReorderAreUnavailable) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig(/*split_supported=*/false)); // MP4
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_FALSE(s.split.split_supported);
    EXPECT_EQ(s.split.availability, MetricAvailability::Unavailable);
    EXPECT_EQ(s.mux.availability, MetricAvailability::Unavailable);
    EXPECT_LT(s.split.seconds_until_auto_split, 0.0);
}

TEST(PipelineDiagnostics, WriteFailureMarksCriticalWithoutThrowing) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    stats.encoded_video_packets = 10;
    // warm up so we are past the "gathering data" window
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 2.0);
    EXPECT_NO_THROW(agg.OnMuxFailure());
    const auto s = agg.BuildSnapshot(At(200), stats, DiagnosticsLifecycle::Recording, 2.2);
    EXPECT_EQ(s.mux.failures, 1u);
    EXPECT_EQ(s.disk.write_failures, 1u);
    EXPECT_EQ(s.health, PipelineHealth::Critical);
}

// ---------------------------------------------------------------------------
// Bottleneck classifier
// ---------------------------------------------------------------------------

// Drive N healthy publishes and return the last snapshot.
RecordingDiagnosticsSnapshot DriveHealthy(PipelineDiagnosticsAggregator& agg, int publishes) {
    auto stats = MakeStats();
    RecordingDiagnosticsSnapshot s;
    for (int i = 0; i < publishes; ++i) {
        stats.video_frames_captured = static_cast<uint64_t>(12 * (i + 1)); // 60 fps
        stats.encoded_video_packets = static_cast<uint64_t>(12 * (i + 1));
        s = agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    return s;
}

TEST(PipelineDiagnosticsClassifier, HealthyPipelineReturnsNone) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = DriveHealthy(agg, 5);
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::None);
    EXPECT_EQ(s.health, PipelineHealth::Good);
}

TEST(PipelineDiagnosticsClassifier, InsufficientEvidenceReturnsUnknown) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    stats.encoded_video_packets = 0; // nothing encoded yet
    const auto s = agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.1);
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::Unknown);
}

TEST(PipelineDiagnosticsClassifier, SustainedEncoderBacklogReturnsVideoEncoder) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    for (int i = 0; i < 8; ++i) {
        agg.OnEncodeSubmitted();
    }
    RecordingDiagnosticsSnapshot s;
    for (int i = 0; i < 6; ++i) {
        // encoded always trails submitted by 5 (backlog) and advances so output_fps > 0
        stats.video_frames_captured = static_cast<uint64_t>(12 * (i + 1));
        stats.encoded_video_packets = static_cast<uint64_t>(3 * (i + 1)); // > 0, but < submitted
        agg.OnEncodeSubmitted();
        agg.OnEncodeSubmitted();
        agg.OnEncodeSubmitted();
        s = agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::VideoEncoder);
    EXPECT_EQ(s.health, PipelineHealth::Warning);
    EXPECT_FALSE(s.bottleneck_reason.empty());
}

TEST(PipelineDiagnosticsClassifier, SustainedCaptureBelowTargetReturnsCapture) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    stats.video_frames_captured = 0;
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 2.0); // baseline

    RecordingDiagnosticsSnapshot s;
    uint64_t emitted = 0;
    for (int i = 1; i <= 5; ++i) {
        emitted += 8; // 8 frames / 200 ms = 40 fps (< 60 * 0.85)
        stats.video_frames_captured = emitted;
        stats.encoded_video_packets = emitted;
        s = agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::Capture);
}

TEST(PipelineDiagnosticsClassifier, SustainedDiskLatencyReturnsDisk) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    RecordingDiagnosticsSnapshot s;
    for (int i = 0; i < 6; ++i) {
        stats.video_frames_captured = static_cast<uint64_t>(12 * (i + 1));
        stats.encoded_video_packets = static_cast<uint64_t>(12 * (i + 1));
        agg.OnDiskWrite(At(200 * i), 20.0, 1024); // 20 ms write-call latency (> 8 ms warn)
        s = agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::Disk);
}

TEST(PipelineDiagnosticsClassifier, SustainedMuxQueueReturnsMuxer) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    RecordingDiagnosticsSnapshot s;
    for (int i = 0; i < 6; ++i) {
        stats.video_frames_captured = static_cast<uint64_t>(12 * (i + 1));
        stats.encoded_video_packets = static_cast<uint64_t>(12 * (i + 1));
        agg.OnVideoQueueDepth(16); // >= mux_queue_warn (8), writes healthy
        s = agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::Muxer);
}

TEST(PipelineDiagnosticsClassifier, SingleSpikeDoesNotTrigger) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();
    RecordingDiagnosticsSnapshot s;
    for (int i = 0; i < 6; ++i) {
        stats.video_frames_captured = static_cast<uint64_t>(12 * (i + 1));
        stats.encoded_video_packets = static_cast<uint64_t>(12 * (i + 1));
        // single transient spike on iteration 2 only
        agg.OnVideoQueueDepth(i == 2 ? 32u : 0u);
        s = agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::None);
}

TEST(PipelineDiagnosticsClassifier, NotRecordingClearsBottleneck) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // Sustain a muxer condition while recording...
    auto stats = MakeStats();
    for (int i = 0; i < 6; ++i) {
        stats.video_frames_captured = static_cast<uint64_t>(12 * (i + 1));
        stats.encoded_video_packets = static_cast<uint64_t>(12 * (i + 1));
        agg.OnVideoQueueDepth(16);
        (void)agg.BuildSnapshot(At(200 * i), stats, DiagnosticsLifecycle::Recording, 0.2 * i + 2.0);
    }
    // ...then stop: bottleneck must clear immediately.
    const auto s = agg.BuildSnapshot(At(2000), stats, DiagnosticsLifecycle::Stopping, 3.5);
    EXPECT_EQ(s.bottleneck, PipelineBottleneck::None);
}

// ---------------------------------------------------------------------------
// Static config derivation
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, StaticConfigDerivesSourceTypeAndSplitSupport) {
    RecorderConfig cfg;
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.container = Container::WebM;
    const auto a = MakeDiagnosticsStaticConfig(cfg);
    EXPECT_EQ(a.source_type, CaptureSourceType::Display);
    EXPECT_TRUE(a.split_supported);

    cfg.crop_region = CaptureRegion{0, 0, 640, 480};
    EXPECT_EQ(MakeDiagnosticsStaticConfig(cfg).source_type, CaptureSourceType::Region);

    cfg.crop_region.reset();
    cfg.target.kind = CaptureTarget::Kind::Window;
    EXPECT_EQ(MakeDiagnosticsStaticConfig(cfg).source_type, CaptureSourceType::Window);

    cfg.container = Container::Mp4;
    EXPECT_FALSE(MakeDiagnosticsStaticConfig(cfg).split_supported);
}

TEST(PipelineDiagnostics, SessionGuardRejectsOlderGeneration) {
    DiagnosticsSessionGuard guard;
    RecordingDiagnosticsSnapshot s;

    s.session_generation = 1;
    EXPECT_TRUE(guard.Accept(s)); // first session accepted
    s.session_generation = 1;
    EXPECT_TRUE(guard.Accept(s)); // same session still accepted
    s.session_generation = 2;
    EXPECT_TRUE(guard.Accept(s)); // newer session accepted
    s.session_generation = 1;
    EXPECT_FALSE(guard.Accept(s)); // stale session-1 snapshot rejected after session-2
    EXPECT_EQ(guard.max_generation(), 2u);
}

TEST(PipelineDiagnostics, SplitTriggerMapping) {
    EXPECT_EQ(ToDiagnosticsSplitTrigger(SplitTriggerSource::ManualButton), DiagnosticsSplitTrigger::ManualButton);
    EXPECT_EQ(ToDiagnosticsSplitTrigger(SplitTriggerSource::Hotkey), DiagnosticsSplitTrigger::Hotkey);
    EXPECT_EQ(ToDiagnosticsSplitTrigger(SplitTriggerSource::AutomaticDuration),
              DiagnosticsSplitTrigger::AutomaticDuration);
    EXPECT_EQ(ToDiagnosticsSplitTrigger(SplitTriggerSource::AutomaticSize), DiagnosticsSplitTrigger::AutomaticSize);
}

// ---------------------------------------------------------------------------
// A/V drift (v0.8.0-C)
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, AvDriftInitiallyZeroAndUnavailable) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 0.0);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Unavailable);
}

TEST(PipelineDiagnostics, AvDriftReportsMeasuredClockDrift) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // The audio worker reports its smoothed device-clock-vs-QPC estimate.
    // Positive = audio leads video.
    agg.OnAudioClockSlaving(0, 2.5, 2.5, 0.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 2.5);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Available);
}

TEST(PipelineDiagnostics, AvDriftLatestEstimatePerTrackWins) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAudioClockSlaving(0, 2.5, 2.5, 0.0);
    agg.OnAudioClockSlaving(0, 3.0, 3.0, 0.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 3.0);
}

TEST(PipelineDiagnostics, AvDriftLargestMagnitudeAcrossTracks_Signed) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // Two device-backed tracks (e.g. SYS render endpoint + MIC capture
    // endpoint) can drift independently; surface the worst one, signed.
    agg.OnAudioClockSlaving(0, 2.0, 2.0, 0.0);
    agg.OnAudioClockSlaving(1, -5.0, -5.0, 0.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, -5.0);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Available);
}

TEST(PipelineDiagnostics, AvDriftOutOfRangeTrackIgnored) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAudioClockSlaving(99, 42.0, 42.0, 0.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Unavailable);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 0.0);
}

TEST(PipelineDiagnostics, AvDriftResetClearsState) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAudioClockSlaving(0, 4.0, 4.0, 0.0);
    // After Reset the per-track estimates must clear.
    agg.Reset(2, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Unavailable);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 0.0);
}

// ---------------------------------------------------------------------------
// Clock slaving: residual is surfaced as av_drift_ms; raw drift, ppm and the
// active flag ride alongside.
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, ClockSlaving_SurfacesResidualWithRawAndPpm) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // Raw drift 20 ms, compensated down to a 6 ms residual at 100 ppm.
    agg.OnAudioClockSlaving(0, 20.0, 6.0, 100.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 6.0);      // the file's real misalignment
    EXPECT_DOUBLE_EQ(s.av_drift_raw_ms, 20.0); // the uncorrected measured drift
    EXPECT_DOUBLE_EQ(s.clock_slaving_ppm, 100.0);
    EXPECT_TRUE(s.clock_slaving_active);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Available);
}

TEST(PipelineDiagnostics, ClockSlaving_InactiveWhenNotEngaged) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAudioClockSlaving(0, 3.0, 3.0, 0.0); // raw == residual, no ppm
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_FALSE(s.clock_slaving_active);
    EXPECT_DOUBLE_EQ(s.clock_slaving_ppm, 0.0);
}

TEST(PipelineDiagnostics, ClockSlaving_SelectsLargestResidualTrack) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // Track 0 has a big raw drift but is well-corrected (small residual); track 1
    // has a smaller raw drift but a larger residual. The larger residual wins,
    // and its raw + ppm come with it.
    agg.OnAudioClockSlaving(0, 40.0, 4.0, 480.0);
    agg.OnAudioClockSlaving(1, 12.0, 9.0, 120.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 9.0);
    EXPECT_DOUBLE_EQ(s.av_drift_raw_ms, 12.0);
    EXPECT_DOUBLE_EQ(s.clock_slaving_ppm, 120.0);
    EXPECT_TRUE(s.clock_slaving_active);
}

// ---------------------------------------------------------------------------
// Disk-fill ETA (v0.8.0-C)
// ---------------------------------------------------------------------------

TEST(PipelineDiagnostics, DiskFillEtaUnavailableWithNoThroughputOrNoFreeBytes) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();

    // No throughput, no free bytes -> -1
    const auto s1 = agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_LT(s1.disk_fill_eta_seconds, 0.0);

    // Free bytes known but throughput still zero (no baseline yet) -> -1
    agg.UpdateFreeDiskBytes(1024u * 1024u * 1024u); // 1 GiB
    const auto s2 = agg.BuildSnapshot(At(200), stats, DiagnosticsLifecycle::Recording, 0.2);
    EXPECT_LT(s2.disk_fill_eta_seconds, 0.0);
}

TEST(PipelineDiagnostics, DiskFillEtaComputedCorrectly) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();

    // Establish baseline
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);

    // Write 100 MiB over 1 second => 100 MiB/s throughput.
    // Free space = 1000 MiB => ETA = 1000 / 100 = 10 seconds.
    agg.OnDiskWrite(At(500), 1.0, 100u * 1024u * 1024u);
    agg.UpdateFreeDiskBytes(1000u * 1024u * 1024u);
    const auto s = agg.BuildSnapshot(At(1000), stats, DiagnosticsLifecycle::Recording, 1.0);
    EXPECT_NEAR(s.disk_fill_eta_seconds, 10.0, 0.1);
}

TEST(PipelineDiagnostics, DiskFillEtaUnavailableWhenFreeBytesZero) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    auto stats = MakeStats();

    // Establish baseline
    (void)agg.BuildSnapshot(At(0), stats, DiagnosticsLifecycle::Recording, 0.0);

    // throughput is positive but free bytes == 0 -> -1
    agg.OnDiskWrite(At(500), 1.0, 50u * 1024u * 1024u);
    agg.UpdateFreeDiskBytes(0u);
    const auto s = agg.BuildSnapshot(At(1000), stats, DiagnosticsLifecycle::Recording, 1.0);
    EXPECT_LT(s.disk_fill_eta_seconds, 0.0);
}

// ---------------------------------------------------------------------------
// Capture-card live wiring: acquire / vpblt / mux-process CPU-timing windows
// ---------------------------------------------------------------------------

TEST(CaptureCardWiring, AcquireWindowAverageAndPeak) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAcquireLatency(At(0), 1.0);
    agg.OnAcquireLatency(At(10), 3.0);
    agg.OnAcquireLatency(At(20), 2.0);
    const auto s = agg.BuildSnapshot(At(30), MakeStats(), DiagnosticsLifecycle::Recording, 0.03);
    EXPECT_EQ(s.capture.acquire_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.capture.acquire_average_ms, 2.0);
    EXPECT_DOUBLE_EQ(s.capture.acquire_peak_ms, 3.0);
    EXPECT_DOUBLE_EQ(s.capture.acquire_latest_ms, 2.0);
}

TEST(CaptureCardWiring, AcquireUnavailableWhenNoSamples) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.acquire_availability, MetricAvailability::Unavailable);
    EXPECT_DOUBLE_EQ(s.capture.acquire_average_ms, 0.0);
}

TEST(CaptureCardWiring, VpbltWindowFoldsIntoCompositor) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnVpbltSubmit(At(0), 0.4);
    agg.OnVpbltSubmit(At(16), 0.6);
    const auto s = agg.BuildSnapshot(At(20), MakeStats(), DiagnosticsLifecycle::Recording, 0.02);
    EXPECT_EQ(s.compositor.vpblt_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.compositor.vpblt_average_ms, 0.5);
    EXPECT_DOUBLE_EQ(s.compositor.vpblt_peak_ms, 0.6);
}

TEST(CaptureCardWiring, MuxProcessWindow) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnMuxLatency(At(0), 0.2);
    agg.OnMuxLatency(At(5), 0.8);
    const auto s = agg.BuildSnapshot(At(10), MakeStats(), DiagnosticsLifecycle::Recording, 0.01);
    EXPECT_EQ(s.mux.process_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.mux.process_average_ms, 0.5);
    EXPECT_DOUBLE_EQ(s.mux.process_peak_ms, 0.8);
}

TEST(CaptureCardWiring, ResetClearsNewWindows) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnAcquireLatency(At(0), 5.0);
    agg.Reset(2, MakeConfig()); // new session
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.capture.acquire_availability, MetricAvailability::Unavailable);
}

// ---------------------------------------------------------------------------
// Peak A/V drift (single source of truth for the UI and the session report)
// ---------------------------------------------------------------------------

TEST(PeakAvDrift, UnavailableUntilDriftIsMeasured) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.5);
    EXPECT_EQ(s.peak_av_drift_availability, MetricAvailability::Unavailable);
}

TEST(PeakAvDrift, RunningMaximumOfMagnitudeIsMonotonic) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());

    agg.OnAudioClockSlaving(0, 3.0, 3.0, 0.0);
    auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.5);
    EXPECT_EQ(s.peak_av_drift_availability, MetricAvailability::Available);
    EXPECT_DOUBLE_EQ(s.peak_av_drift_ms, 3.0);

    // A larger-magnitude (negative) drift raises the peak.
    agg.OnAudioClockSlaving(0, -7.0, -7.0, 0.0);
    s = agg.BuildSnapshot(At(100), MakeStats(), DiagnosticsLifecycle::Recording, 0.6);
    EXPECT_DOUBLE_EQ(s.peak_av_drift_ms, 7.0);

    // A smaller drift does not lower the peak.
    agg.OnAudioClockSlaving(0, 1.0, 1.0, 0.0);
    s = agg.BuildSnapshot(At(200), MakeStats(), DiagnosticsLifecycle::Recording, 0.7);
    EXPECT_DOUBLE_EQ(s.peak_av_drift_ms, 7.0);

    // Reset clears the peak for a fresh session.
    agg.Reset(2, MakeConfig());
    s = agg.BuildSnapshot(At(300), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.peak_av_drift_availability, MetricAvailability::Unavailable);
}

// ---------------------------------------------------------------------------
// Encoder init info passthrough
// ---------------------------------------------------------------------------

TEST(EncoderInit, InvalidUntilSetThenCarriedOnSnapshots) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());

    auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_FALSE(s.encoder_init.valid);

    EncoderInitInfo info;
    info.valid = true;
    info.codec = VideoCodec::Av1Nvenc;
    info.preset = NvencPreset::P5;
    info.rc_mode = RateControlMode::VariableBitrate;
    info.target_bitrate_kbps = 20000;
    info.gop_length = 120;
    info.bit_depth = BitDepth::Bit10;
    agg.SetEncoderInitInfo(info);

    s = agg.BuildSnapshot(At(100), MakeStats(), DiagnosticsLifecycle::Recording, 0.5);
    EXPECT_TRUE(s.encoder_init.valid);
    EXPECT_EQ(s.encoder_init.preset, NvencPreset::P5);
    EXPECT_EQ(s.encoder_init.rc_mode, RateControlMode::VariableBitrate);
    EXPECT_EQ(s.encoder_init.target_bitrate_kbps, 20000u);
    EXPECT_EQ(s.encoder_init.gop_length, 120u);
    EXPECT_EQ(s.encoder_init.bit_depth, BitDepth::Bit10);

    // Carried unchanged on a later snapshot.
    s = agg.BuildSnapshot(At(200), MakeStats(), DiagnosticsLifecycle::Recording, 0.6);
    EXPECT_TRUE(s.encoder_init.valid);
    EXPECT_EQ(s.encoder_init.gop_length, 120u);

    // Cleared on Reset.
    agg.Reset(2, MakeConfig());
    s = agg.BuildSnapshot(At(300), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_FALSE(s.encoder_init.valid);
}

// ---------------------------------------------------------------------------
// Per-stage full percentile exposure (mean/p50/p95/p99/max) via SampleStage
// ---------------------------------------------------------------------------

TEST(StageStats, SampleStageEmptyWindowIsAllZero) {
    RollingTimeWindow w(64, milliseconds(2000));
    const StageWindowStats s = SampleStage(w, At(0));
    EXPECT_EQ(s.samples, 0u);
    EXPECT_DOUBLE_EQ(s.mean_ms, 0.0);
    EXPECT_DOUBLE_EQ(s.p50_ms, 0.0);
    EXPECT_DOUBLE_EQ(s.p95_ms, 0.0);
    EXPECT_DOUBLE_EQ(s.p99_ms, 0.0);
    EXPECT_DOUBLE_EQ(s.max_ms, 0.0);
}

TEST(StageStats, SampleStageComputesMeanPercentilesMax) {
    RollingTimeWindow w(256, milliseconds(100000));
    for (int i = 1; i <= 100; ++i) {
        w.Add(At(i), static_cast<double>(i));
    }
    const StageWindowStats s = SampleStage(w, At(100));
    EXPECT_EQ(s.samples, 100u);
    EXPECT_NEAR(s.mean_ms, 50.5, 1e-9);
    EXPECT_DOUBLE_EQ(s.max_ms, 100.0);
    // Nearest-rank on a 0-based sorted index of 100 elements:
    // p50 -> round(0.50*99)=50 -> 51; p95 -> round(0.95*99)=94 -> 95;
    // p99 -> round(0.99*99)=98 -> 99.
    EXPECT_DOUBLE_EQ(s.p50_ms, 51.0);
    EXPECT_DOUBLE_EQ(s.p95_ms, 95.0);
    EXPECT_DOUBLE_EQ(s.p99_ms, 99.0);
}

TEST(StageStats, SummarizeStageEmptyHistogram) {
    LatencyHistogram h;
    const StageHistogramSummary s = SummarizeStage(h);
    EXPECT_EQ(s.count, 0u);
    EXPECT_DOUBLE_EQ(s.p50_ms, 0.0);
}

// ---------------------------------------------------------------------------
// GPU-execution-time stages (kept distinct from CPU-submission windows)
// ---------------------------------------------------------------------------

TEST(GpuStageTiming, CompositionGpuIsDistinctFromCompositionCpu) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 20; ++i) {
        agg.OnCompositorSubmit(At(i), 0.3, true); // cheap CPU submit
        agg.OnCompositionGpuTime(At(i), 4.0);     // real GPU work is more expensive
    }
    const auto p = agg.SamplePerfWindow(At(20));
    EXPECT_NEAR(p.composition_cpu.p50_ms, 0.3, 1e-9);
    EXPECT_NEAR(p.composition_gpu.p50_ms, 4.0, 1e-9);
    EXPECT_EQ(p.composition_cpu.samples, 20u);
    EXPECT_EQ(p.composition_gpu.samples, 20u);
}

TEST(GpuStageTiming, TonemapVpbltAndUploadGpuWindowsFeedIndependently) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 10; ++i) {
        agg.OnHdrTonemapGpuTime(At(i), 2.0);
        agg.OnVpbltSubmit(At(i), 0.5);     // CPU submit for RGB->YUV
        agg.OnRgbToYuvGpuTime(At(i), 1.5); // GPU exec for RGB->YUV
        agg.OnWebcamUploadGpuTime(At(i), 0.7);
    }
    const auto p = agg.SamplePerfWindow(At(10));
    EXPECT_NEAR(p.hdr_tonemap_gpu.mean_ms, 2.0, 1e-9);
    EXPECT_NEAR(p.rgb_to_yuv_cpu.mean_ms, 0.5, 1e-9);
    EXPECT_NEAR(p.rgb_to_yuv_gpu.mean_ms, 1.5, 1e-9);
    EXPECT_NEAR(p.webcam_upload_gpu.mean_ms, 0.7, 1e-9);
}

TEST(GpuStageTiming, WholeSessionHistogramsAccumulateAndReset) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 15; ++i) {
        agg.OnCompositionGpuTime(At(i), 3.0);
        agg.OnHdrTonemapGpuTime(At(i), 2.0);
        agg.OnRgbToYuvGpuTime(At(i), 1.0);
        agg.OnAcquireLatency(At(i), 0.4);
        agg.OnMuxLatency(At(i), 0.2);
    }
    auto sum = agg.BuildPerfSummary();
    EXPECT_EQ(sum.composition_gpu.count, 15u);
    EXPECT_EQ(sum.hdr_tonemap_gpu.count, 15u);
    EXPECT_EQ(sum.rgb_to_yuv_gpu.count, 15u);
    EXPECT_EQ(sum.acquire.count, 15u);
    EXPECT_EQ(sum.mux_process.count, 15u);
    const std::size_t b3 = recorder_core::LatencyHistogram::BucketIndex(3.0);
    EXPECT_EQ(sum.composition_gpu.buckets[b3], 15u);

    agg.Reset(2, MakeConfig());
    sum = agg.BuildPerfSummary();
    EXPECT_EQ(sum.composition_gpu.count, 0u);
    EXPECT_EQ(sum.acquire.count, 0u);
}

// ---------------------------------------------------------------------------
// New CPU-timed stages: webcam convert, preview copy, mux queue delay
// ---------------------------------------------------------------------------

TEST(NewCpuStages, WebcamConvertPreviewCopyAndMuxQueueDelay) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    for (int i = 0; i < 8; ++i) {
        agg.OnWebcamConvert(At(i), 1.2);
        agg.OnPreviewCopy(At(i), 0.6);
        agg.OnMuxQueueDelay(At(i), 5.0);
    }
    const auto p = agg.SamplePerfWindow(At(8));
    EXPECT_NEAR(p.webcam_convert.p50_ms, 1.2, 1e-9);
    EXPECT_NEAR(p.preview_copy.p50_ms, 0.6, 1e-9);
    EXPECT_NEAR(p.mux_queue_delay.p50_ms, 5.0, 1e-9);
    const auto sum = agg.BuildPerfSummary();
    EXPECT_EQ(sum.webcam_convert.count, 8u);
    EXPECT_EQ(sum.preview_copy.count, 8u);
    EXPECT_EQ(sum.mux_queue_delay.count, 8u);
}

// ---------------------------------------------------------------------------
// CFR duplicate-frame counter
// ---------------------------------------------------------------------------

TEST(DuplicateFrames, CounterAccumulatesAndResets) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnFrameDuplicated();
    agg.OnFrameDuplicated();
    agg.OnFrameDuplicated();
    EXPECT_EQ(agg.SamplePerfWindow(At(0)).duplicated_frames, 3u);
    EXPECT_EQ(agg.BuildPerfSummary().duplicated_frames, 3u);
    agg.Reset(2, MakeConfig());
    EXPECT_EQ(agg.SamplePerfWindow(At(0)).duplicated_frames, 0u);
}

// ---------------------------------------------------------------------------
// Queue saturation event counter (rising-edge crossings)
// ---------------------------------------------------------------------------

TEST(QueueSaturation, VideoQueueCountsRisingEdgeOnly) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // Default mux_queue_warn == 8.
    agg.OnVideoQueueDepth(2);  // below
    agg.OnVideoQueueDepth(10); // rising edge -> +1
    agg.OnVideoQueueDepth(12); // stays saturated -> no new event
    agg.OnVideoQueueDepth(3);  // drops below -> reset edge
    agg.OnVideoQueueDepth(9);  // rising edge again -> +1
    EXPECT_EQ(agg.SamplePerfWindow(At(0)).queue_saturation_events, 2u);
}

TEST(QueueSaturation, AudioPremuxUsesCriticalRatioOfCapacity) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());  // audio capacity 600, critical ratio 0.9 -> >= 540
    agg.OnAudioPremuxDepth(100); // below
    agg.OnAudioPremuxDepth(540); // exactly at threshold -> +1
    agg.OnAudioPremuxDepth(560); // still saturated -> no new event
    agg.OnAudioPremuxDepth(10);  // drop
    agg.OnAudioPremuxDepth(600); // rising edge -> +1
    EXPECT_EQ(agg.SamplePerfWindow(At(0)).queue_saturation_events, 2u);
}

TEST(QueueSaturation, ResetClearsCounterAndEdgeState) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnVideoQueueDepth(20); // +1, and leaves the edge latched high
    EXPECT_EQ(agg.SamplePerfWindow(At(0)).queue_saturation_events, 1u);
    agg.Reset(2, MakeConfig());
    // After reset the latch is cleared, so the next high depth is a fresh edge.
    agg.OnVideoQueueDepth(20); // +1
    EXPECT_EQ(agg.SamplePerfWindow(At(0)).queue_saturation_events, 1u);
}

// ---------------------------------------------------------------------------
// Retained-frame reuse counters
// ---------------------------------------------------------------------------

TEST(PipelineDiagnosticsAggregator, RetainedFrameCountersRoundTrip) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());

    agg.OnScreenGenerationChanged();
    agg.OnScreenGenerationChanged();
    agg.OnWebcamGenerationChanged();
    agg.OnCursorOnlyCaptureEventIgnored();
    agg.OnPhaseRingCursorOnlyEventIgnored();
    agg.OnFullComposition();
    agg.OnReusedYuvFrame();
    agg.OnReusedYuvFrame();
    agg.OnYuvSlotCopy();
    agg.OnYuvSlotCopySkipped();
    agg.OnYuvSlotCopySkipped();
    agg.OnYuvSlotCopySkipped();

    const auto snap = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);

    EXPECT_EQ(snap.screen_generation_changes, 2u);
    EXPECT_EQ(snap.webcam_generation_changes, 1u);
    EXPECT_EQ(snap.cursor_only_capture_events_ignored, 1u);
    EXPECT_EQ(snap.phase_ring_cursor_only_events_ignored, 1u);
    EXPECT_EQ(snap.full_compositions, 1u);
    EXPECT_EQ(snap.reused_yuv_frames, 2u);
    EXPECT_EQ(snap.yuv_slot_copies, 1u);
    EXPECT_EQ(snap.yuv_slot_copies_skipped, 3u);
}

} // namespace
