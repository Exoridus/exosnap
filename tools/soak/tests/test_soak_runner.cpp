#include "soak_runner.h"
#include "soak_stop_control.h"
#include "synthetic_audio_source.h"
#include "synthetic_output_fault.h"

#include <gtest/gtest.h>

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
