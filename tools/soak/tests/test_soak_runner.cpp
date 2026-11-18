#include "soak_runner.h"
#include "soak_stop_control.h"
#include "synthetic_audio_source.h"
#include "synthetic_output_fault.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>

namespace {

class ConstantSampler final : public exosnap::soak::IProcessSampler {
  public:
    exosnap::soak::ProcessMetrics Sample() override {
        return {};
    }
};

} // namespace

TEST(SoakRunner, UnwritableTimelineCannotStartAnApparentlyValidRun) {
    ConstantSampler sampler;
    exosnap::soak::SoakRunner runner({}, sampler, std::filesystem::temp_directory_path().string());
    EXPECT_THROW(runner.Start(0.01), std::runtime_error);
    runner.Stop();
}

TEST(SoakRunner, AbortCallbackSeesThePublishedReasonWithoutAnotherDiagnosticsCallback) {
    ConstantSampler sampler;
    exosnap::soak::SoakThresholds thresholds;
    thresholds.sustained_samples = 1;
    std::promise<std::string> reason;
    auto observed = reason.get_future();
    exosnap::soak::SoakRunner* active = nullptr;
    std::atomic<unsigned> calls{0};
    exosnap::soak::SoakRunner runner(thresholds, sampler, "", [&] {
        if (calls.fetch_add(1) == 0)
            reason.set_value(active->abort_decision().reason);
    });
    active = &runner;
    exosnap::engine::RecordingDiagnosticsSnapshot diagnostics;
    diagnostics.health = exosnap::engine::PipelineHealth::Critical;
    runner.OnDiagnostics(diagnostics);
    runner.Start(0.01);
    const auto status = observed.wait_for(std::chrono::seconds(2));
    runner.Stop();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(observed.get().empty());
    EXPECT_EQ(calls.load(), 1u);
}

TEST(SoakRunner, SamplerFailureIsReportedAndRequestsStop) {
    class FailingSampler final : public exosnap::soak::IProcessSampler {
      public:
        exosnap::soak::ProcessMetrics Sample() override {
            throw std::runtime_error("process sample unavailable");
        }
    } sampler;
    std::promise<void> stopped;
    auto observed = stopped.get_future();
    exosnap::soak::SoakRunner runner({}, sampler, "", [&] { stopped.set_value(); });
    runner.Start(0.01);
    const auto status = observed.wait_for(std::chrono::seconds(2));
    runner.Stop();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(runner.error().empty());
    EXPECT_TRUE(runner.aborted());
}

TEST(SoakStopControl, FinishingAnEarlySessionCancelsAnHoursLongDeadline) {
    exosnap::soak::SoakStopControl control;
    std::atomic<unsigned> stops{0};
    control.Start(std::chrono::hours(4), [&] { ++stops; });
    control.Complete();
    control.RequestStop();
    EXPECT_EQ(stops.load(), 0u);
}

TEST(SoakStopControl, PendingStopBeforeStartIsDeliveredExactlyOnce) {
    exosnap::soak::SoakStopControl control;
    std::promise<void> stopped;
    auto observed = stopped.get_future();
    std::atomic<unsigned> stops{0};
    control.RequestStop();
    control.RequestStop();
    control.Start(std::chrono::hours(4), [&] {
        ++stops;
        stopped.set_value();
    });
    const auto status = observed.wait_for(std::chrono::seconds(2));
    control.Complete();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(stops.load(), 1u);
}

TEST(SoakStopControl, DeadlineRequestsStopWithoutAnEngineCallback) {
    exosnap::soak::SoakStopControl control;
    std::promise<void> stopped;
    auto observed = stopped.get_future();
    control.Start(std::chrono::milliseconds(1), [&] { stopped.set_value(); });
    const auto status = observed.wait_for(std::chrono::seconds(2));
    control.Complete();
    EXPECT_EQ(status, std::future_status::ready);
}

TEST(SoakStopControl, SuccessfulSessionCannotHideAnInvalidOutputOrMissingEvidence) {
    EXPECT_EQ(exosnap::soak::SoakExitCode(true, true, true, false), 0);
    EXPECT_EQ(exosnap::soak::SoakExitCode(true, false, true, false), 2);
    EXPECT_EQ(exosnap::soak::SoakExitCode(true, true, false, false), 2);
    EXPECT_EQ(exosnap::soak::SoakExitCode(false, false, true, false), 1);
    EXPECT_EQ(exosnap::soak::SoakExitCode(true, true, true, true), 3);
}

TEST(SyntheticOutputFault, OnlyTheConfiguredOccurrenceFiresAndCanBeReplayed) {
    using namespace exosnap::engine::testutil;
    for (unsigned replay = 0; replay < 2; ++replay) {
        SyntheticOutputFault fault({SyntheticOutputFaultPoint::Trailer, 3});
        EXPECT_FALSE(fault.Visit(SyntheticOutputFaultPoint::Cluster));
        EXPECT_FALSE(fault.Visit(SyntheticOutputFaultPoint::Trailer));
        EXPECT_FALSE(fault.Visit(SyntheticOutputFaultPoint::Payload));
        EXPECT_FALSE(fault.Visit(SyntheticOutputFaultPoint::Trailer));
        EXPECT_TRUE(fault.Visit(SyntheticOutputFaultPoint::Trailer));
        EXPECT_FALSE(fault.Visit(SyntheticOutputFaultPoint::Trailer));
        EXPECT_EQ(fault.visits(), 4u);
        EXPECT_TRUE(fault.triggered());
    }
}

TEST(SyntheticOutputFault, InvalidScheduleCannotSilentlyDisableTheFault) {
    using namespace exosnap::engine::testutil;
    EXPECT_THROW((SyntheticOutputFault({SyntheticOutputFaultPoint::Trailer, 0})), std::invalid_argument);
    EXPECT_EQ(ParseSyntheticOutputFault("cluster"), SyntheticOutputFaultPoint::Cluster);
    EXPECT_EQ(ParseSyntheticOutputFault("trailer"), SyntheticOutputFaultPoint::Trailer);
    EXPECT_FALSE(ParseSyntheticOutputFault("unknown"));
}

TEST(SyntheticAudioSource, DeclaredDurationDoesNotAllocateAllFuturePcm) {
    using exosnap::engine::testutil::SyntheticAudioSource;
    // The source is a streaming generator. A large packet count must cost no
    // more storage than a short run, including before an immediate cancel.
    EXPECT_NO_THROW((SyntheticAudioSource(nullptr, std::numeric_limits<size_t>::max())));
}

TEST(SyntheticAudioSource, ProducesSamplesAndSignalsOnlyAfterTheDeclaredTail) {
    auto drained = std::make_shared<std::atomic<bool>>(false);
    exosnap::engine::testutil::SyntheticAudioSource source(drained, 2);
    std::string error;
    ASSERT_TRUE(source.Init(error));
    for (unsigned packet = 0; packet < 2; ++packet) {
        EXPECT_FALSE(drained->load());
        EXPECT_EQ(source.PendingFrameCount(), 960u);
        exosnap::engine::RawAudioBuffer buffer;
        ASSERT_TRUE(source.AcquireBuffer(buffer, error));
        EXPECT_EQ(buffer.num_frames, 960u);
        EXPECT_FLOAT_EQ(reinterpret_cast<const float*>(buffer.bytes)[0], 0.1f);
        source.ReleaseBuffer();
    }
    EXPECT_TRUE(drained->load());
    EXPECT_EQ(source.PendingFrameCount(), 0u);
}

TEST(SyntheticAudioSource, RetainedWorkerKeepsItsCompletionSignalAlive) {
    auto drained = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<std::atomic<bool>> retained = drained;
    {
        exosnap::engine::testutil::SyntheticAudioSource source(drained, 1);
        drained.reset();
        ASSERT_FALSE(retained.expired());
        std::string error;
        ASSERT_TRUE(source.Init(error));
        exosnap::engine::RawAudioBuffer buffer;
        ASSERT_TRUE(source.AcquireBuffer(buffer, error));
        source.ReleaseBuffer();
        EXPECT_TRUE(retained.lock()->load());
    }
    EXPECT_TRUE(retained.expired());
}

TEST(SoakRunner, CanonicalLossAndEveryCategoryReachTheTimelineAndSummary) {
    ConstantSampler sampler;
    exosnap::soak::SoakThresholds thresholds;
    thresholds.sustained_samples = 1;
    exosnap::soak::SoakRunner runner(thresholds, sampler, "");
    exosnap::engine::RecordingDiagnosticsSnapshot diagnostics;
    diagnostics.capture.frames_emitted = 10000;
    diagnostics.capture.frames_dropped_coalesced = 100000;
    diagnostics.capture.frames_dropped_cfr = 20000;
    diagnostics.capture.frames_dropped_ring_eviction = 30000;
    diagnostics.capture.frames_dropped_backpressure = 2;
    diagnostics.capture.frames_dropped_processing_failure = 3;
    diagnostics.video_encoder.undrained_frames = 4;
    runner.OnDiagnostics(diagnostics);
    runner.Start(0.01);
    runner.Stop();
    const auto row = runner.timeline().back();
    EXPECT_EQ(row.real_frame_loss, diagnostics.real_frame_loss());
    EXPECT_EQ(row.frames_dropped_ring_eviction, 30000u);
    EXPECT_EQ(row.encoder_undrained_frames, 4u);
    EXPECT_EQ(runner.Summarize().real_frame_loss, 9u);
    EXPECT_TRUE(runner.aborted());
}

TEST(SoakRunner, StorageWatchRequestsTheExistingStopAndWritesItsObservation) {
    ConstantSampler sampler;
    std::promise<void> stopped;
    auto observed = stopped.get_future();
    exosnap::soak::SoakRunner runner({}, sampler, "", [&] { stopped.set_value(); });
    exosnap::soak::StorageWatchConfig config{30.0, 0.01, 300, 500, 100, false};
    std::atomic<unsigned> samples{0};
    runner.ConfigureStorageWatch(config, [&](double elapsed) {
        const auto number = samples.fetch_add(1);
        return exosnap::soak::StorageObservation{elapsed, number == 0 ? 100000u : 400u, 100000u, 0u};
    });
    runner.Start(0.01);
    const auto status = observed.wait_for(std::chrono::seconds(2));
    runner.Stop();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(runner.aborted());
    EXPECT_NE(runner.abort_decision().reason.find("storage"), std::string::npos);
    const auto summary = runner.Summarize();
    ASSERT_TRUE(summary.storage);
    EXPECT_EQ(summary.storage->observation.output_free_bytes, 400u);
    EXPECT_NE(exosnap::soak::SummaryToJson(summary, {}).find("output_free_bytes"), std::string::npos);
    EXPECT_NE(exosnap::soak::SummaryToMarkdown(summary, {}).find("Storage watch"), std::string::npos);
}

TEST(SoakRunner, StorageObservationFailureRequestsStopInsteadOfDisablingProtection) {
    ConstantSampler sampler;
    std::promise<void> stopped;
    auto observed = stopped.get_future();
    exosnap::soak::SoakRunner runner({}, sampler, "", [&] { stopped.set_value(); });
    runner.ConfigureStorageWatch({30.0, 60.0, 300, 500, 100, false},
                                 [](double elapsed) { return exosnap::soak::StorageObservation{elapsed}; });
    runner.Start(0.01);
    const auto status = observed.wait_for(std::chrono::seconds(2));
    runner.Stop();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(runner.aborted());
    EXPECT_NE(runner.abort_decision().reason.find("unavailable"), std::string::npos);
}

TEST(SoakRunner, SummaryRetainsTheFirstAbortAfterStorageRecovers) {
    ConstantSampler sampler;
    std::promise<void> recovered;
    auto observed = recovered.get_future();
    exosnap::soak::SoakRunner runner({}, sampler, "");
    std::atomic<unsigned> samples{0};
    runner.ConfigureStorageWatch({30.0, 0.01, 300, 500, 100, false}, [&](double elapsed) {
        const auto number = samples.fetch_add(1);
        if (number == 2)
            recovered.set_value();
        const uint64_t free = number == 0 ? 100000ULL : (number == 1 ? 400ULL : 100000000000ULL);
        return exosnap::soak::StorageObservation{elapsed, free, 100000u, 0u};
    });
    runner.Start(0.01);
    const auto status = observed.wait_for(std::chrono::seconds(2));
    runner.Stop();
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(runner.aborted());
    const auto history = runner.timeline();
    ASSERT_FALSE(history.empty());
    ASSERT_TRUE(history.back().storage.has_value());
    EXPECT_TRUE(history.back().storage->stop_reason.empty());
    EXPECT_EQ(exosnap::soak::SoakAbortPolicy{}.Evaluate(history).verdict, exosnap::soak::SoakVerdict::Continue);
    EXPECT_EQ(runner.Summarize().advisory_verdict.verdict, exosnap::soak::SoakVerdict::Abort);
    EXPECT_EQ(runner.Summarize().advisory_verdict.reason, runner.abort_decision().reason);
}

TEST(SoakRunner, ResourceAndAudioObservationsKeepAvailabilityAndAdapterIdentity) {
    class ResourceSampler final : public exosnap::soak::IProcessSampler {
      public:
        exosnap::soak::ProcessMetrics Sample() override {
            return {};
        }
        exosnap::soak::ProcessMetrics SampleForAdapter(int64_t adapter) override {
            observed_adapter = adapter;
            exosnap::soak::ProcessMetrics result;
            result.process_cpu_percent = 5.0;
            result.system_cpu_percent = 15.0;
            result.gpu_utilization_percent = 20.0;
            result.encoder_utilization_percent = 25.0;
            result.process_gpu_memory_bytes = 1048576;
            return result;
        }
        int64_t observed_adapter = 0;
    } sampler;
    exosnap::soak::SoakRunner runner({}, sampler, "");
    exosnap::engine::RecordingDiagnosticsSnapshot diagnostics;
    diagnostics.valid = true;
    diagnostics.capture_adapter_luid = 7;
    diagnostics.av_drift_availability = exosnap::engine::MetricAvailability::Available;
    diagnostics.av_drift_ms = 5.0;
    diagnostics.av_drift_raw_ms = 70.0;
    diagnostics.clock_slaving_ppm = 250.0;
    diagnostics.clock_slaving_active = true;
    diagnostics.clock_slaving_saturated = true;
    diagnostics.audio.sample_rate = 48000;
    diagnostics.audio.discontinuity_frames_total = 48;
    diagnostics.audio.discontinuity_frames_longest = 24;
    diagnostics.audio.source_degraded_occurred = true;
    diagnostics.audio.track_count = 2;
    diagnostics.audio.resampler_drain_recorded[0] = true;
    diagnostics.audio.resampler_undrained_frames[0] = 0;
    runner.OnDiagnostics(diagnostics);
    runner.Start(0.01);
    runner.Stop();
    const auto row = nlohmann::json::parse(exosnap::soak::SampleToJsonLine(runner.timeline().back()));
    EXPECT_EQ(sampler.observed_adapter, 7);
    EXPECT_EQ(row["av_drift_raw_ms"], 70.0);
    EXPECT_EQ(row["clock_slaving_ppm"], 250.0);
    EXPECT_EQ(row["clock_slaving_active"], true);
    EXPECT_EQ(row["clock_slaving_saturated"], true);
    EXPECT_EQ(row["process_cpu_percent"], 5.0);
    EXPECT_EQ(row["system_cpu_percent"], 15.0);
    EXPECT_EQ(row["gpu_utilization_percent"], 20.0);
    EXPECT_EQ(row["encoder_utilization_percent"], 25.0);
    EXPECT_EQ(row["process_gpu_memory_bytes"], 1048576);
    EXPECT_EQ(row["audio_discontinuity_ms_total"], 1.0);
    EXPECT_EQ(row["audio_discontinuity_ms_longest"], 0.5);
    EXPECT_EQ(row["audio_source_degraded_occurred"], true);
    ASSERT_EQ(row["resampler_undrained_frames"].size(), 2u);
    EXPECT_EQ(row["resampler_undrained_frames"][0], 0);
    EXPECT_TRUE(row["resampler_undrained_frames"][1].is_null());
    const auto absent = nlohmann::json::parse(exosnap::soak::SampleToJsonLine({}));
    EXPECT_TRUE(absent["av_drift_raw_ms"].is_null());
    EXPECT_TRUE(absent["clock_slaving_ppm"].is_null());
    EXPECT_TRUE(absent["clock_slaving_active"].is_null());
    EXPECT_TRUE(absent["clock_slaving_saturated"].is_null());
    EXPECT_TRUE(absent["process_cpu_percent"].is_null());
    EXPECT_TRUE(absent["gpu_utilization_percent"].is_null());
    EXPECT_TRUE(absent["audio_discontinuity_ms_total"].is_null());
}

TEST(SoakRunner, ProtectedRunRejectsASamplerSlowerThanTheStorageDeadline) {
    ConstantSampler sampler;
    exosnap::soak::SoakRunner runner({}, sampler, "");
    runner.ConfigureStorageWatch({30.0, 0.01, 300, 500, 100, false}, [](double elapsed) {
        return exosnap::soak::StorageObservation{elapsed, 100000u, 100000u, 0u};
    });
    EXPECT_THROW(runner.Start(1.0), std::invalid_argument);
    runner.Stop();
}
