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

// ---------------------------------------------------------------------------
// Snapshot defaults / lifecycle / generation
// ---------------------------------------------------------------------------

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

TEST(PipelineDiagnostics, AvDriftComputedFromPtsInputs) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    // audio PTS = 120 ms, video PTS = 100 ms => drift = +20 ms (audio leads)
    agg.OnVideoPts(100.0);
    agg.OnAudioPts(120.0);
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 20.0);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Available);
}

TEST(PipelineDiagnostics, AvDriftUnavailableIfOnlyOneStreamHasPts) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnVideoPts(100.0); // only video; no audio PTS yet
    const auto s = agg.BuildSnapshot(At(50), MakeStats(), DiagnosticsLifecycle::Recording, 0.05);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Unavailable);
}

TEST(PipelineDiagnostics, AvDriftResetClearsState) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, MakeConfig());
    agg.OnVideoPts(100.0);
    agg.OnAudioPts(120.0);
    // After Reset, both "have" flags must clear.
    agg.Reset(2, MakeConfig());
    const auto s = agg.BuildSnapshot(At(0), MakeStats(), DiagnosticsLifecycle::Recording, 0.0);
    EXPECT_EQ(s.av_drift_availability, MetricAvailability::Unavailable);
    EXPECT_DOUBLE_EQ(s.av_drift_ms, 0.0);
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

} // namespace
