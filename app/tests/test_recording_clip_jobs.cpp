#include "services/RecordingClipJobs.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

namespace exosnap {
namespace {

RecordingClipRequest MissingSourceRequest(int64_t end_ns) {
    RecordingClipRequest request;
    request.snapshot.token.recording_id.bytes[0] = 1;
    request.requested_end_r_ns = end_ns;
    request.destination = std::filesystem::temp_directory_path() /
                          ("exosnap-clip-queue-" + engine::ToUuid(engine::NewId<engine::JobId>()) + ".mkv");
    return request;
}

TEST(BackgroundStorageBudgetTest, SharedCloseWakesBlockedTransferAndClosesAdmission) {
    BackgroundStorageBudget storage;
    ASSERT_TRUE(storage.Acquire(4ULL * 1024 * 1024));
    std::promise<void> started;
    std::promise<bool> admitted;
    auto result = admitted.get_future();
    std::jthread another_owner([&] {
        started.set_value();
        admitted.set_value(storage.Acquire(4ULL * 1024 * 1024));
    });
    started.get_future().wait();
    storage.Close();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_FALSE(storage.Acquire(1));
}

TEST(BackgroundStorageBudgetTest, CancelledTransferDoesNotConsumeSharedAdmission) {
    BackgroundStorageBudget storage;
    ASSERT_TRUE(storage.Acquire(4ULL * 1024 * 1024));
    std::atomic<bool> cancelled{false};
    std::promise<void> started;
    std::promise<bool> admitted;
    auto result = admitted.get_future();
    std::jthread another_owner([&] {
        started.set_value();
        admitted.set_value(storage.Acquire(4ULL * 1024 * 1024, &cancelled));
    });
    started.get_future().wait();
    cancelled.store(true);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_TRUE(storage.Acquire(1));
}

TEST(BackgroundStorageBudgetTest, RecorderPressurePausesOtherOwnersUntilReleased) {
    BackgroundStorageBudget storage;
    storage.SetPaused(true);
    std::promise<bool> admitted;
    auto result = admitted.get_future();
    std::jthread another_owner([&] { admitted.set_value(storage.Acquire(1)); });
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(40)), std::future_status::timeout);
    storage.SetPaused(false);
    const auto resumed = result.wait_for(std::chrono::seconds(1));
    storage.Close();
    ASSERT_EQ(resumed, std::future_status::ready);
    EXPECT_TRUE(result.get());
}

TEST(RecordingClipJobsTest, ThreeReservationsBoundQueueAndDuplicateRequestReusesJob) {
    RecordingArtifactRegistry registry;
    std::atomic<bool> paused{true};
    RecordingClipJobs clips(registry, {}, [&] { return paused.load(); }, {});
    auto first = MissingSourceRequest(1000000000);
    const auto destination = first.destination;
    const auto accepted = clips.RequestPlan(first);
    ASSERT_TRUE(accepted.succeeded());
    const auto repeated = clips.RequestPlan(first);
    ASSERT_TRUE(repeated.succeeded());
    EXPECT_EQ(*repeated.value(), *accepted.value());
    ASSERT_TRUE(clips.RequestPlan(MissingSourceRequest(2000000000)).succeeded());
    ASSERT_TRUE(clips.RequestPlan(MissingSourceRequest(3000000000)).succeeded());
    const auto overflow = clips.RequestPlan(MissingSourceRequest(4000000000));
    ASSERT_FALSE(overflow.succeeded());
    EXPECT_EQ(overflow.error()->code, engine::EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_EQ(overflow.error()->retryability, engine::Retryability::Transient);
    EXPECT_EQ(clips.Snapshots().size(), 3u);
    EXPECT_FALSE(std::filesystem::exists(destination));
    clips.Shutdown();
    for (const auto& job : clips.Snapshots()) {
        EXPECT_EQ(job.phase, RecordingClipPhase::Cancelled);
        EXPECT_FALSE(job.completed);
        EXPECT_FALSE(job.asset_id);
        EXPECT_EQ(job.reserved_bytes, 0u);
    }
}

TEST(RecordingClipJobsTest, StaleGenerationCannotConfirmOrCancelAnotherJob) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    ASSERT_TRUE(clips.RequestPlan(MissingSourceRequest(1000000000)).succeeded());
    const auto job = clips.Snapshots().front();
    EXPECT_FALSE(clips.Confirm(job.job_id, job.generation + 1));
    EXPECT_FALSE(clips.Cancel(job.job_id, job.generation + 1));
    EXPECT_EQ(clips.Snapshots().front().phase, RecordingClipPhase::Planning);
    EXPECT_TRUE(clips.Cancel(job.job_id, job.generation));
    clips.Shutdown();
    EXPECT_EQ(clips.Snapshots().front().phase, RecordingClipPhase::Cancelled);
}

TEST(RecordingClipJobsTest, PlanningFailurePublishesNeitherMediaNorCompletedAsset) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, {}, {});
    const auto request = MissingSourceRequest(1000000000);
    ASSERT_TRUE(clips.RequestPlan(request).succeeded());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (clips.Snapshots().front().phase != RecordingClipPhase::Failed && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto job = clips.Snapshots().front();
    ASSERT_EQ(job.phase, RecordingClipPhase::Failed);
    EXPECT_FALSE(job.completed);
    EXPECT_FALSE(job.asset_id);
    EXPECT_FALSE(std::filesystem::exists(request.destination));
}

TEST(RecordingClipJobsTest, MarkerMemoryIsRejectedBeforeQueueAdmission) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    auto request = MissingSourceRequest(1000000000);
    request.markers.push_back({0, RecordingMarkerType::General, std::string(2 * 1024 * 1024, 'x')});
    const auto rejected = clips.RequestPlan(std::move(request));
    ASSERT_FALSE(rejected.succeeded());
    EXPECT_EQ(rejected.error()->code, engine::EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_TRUE(clips.Snapshots().empty());
    clips.Shutdown();
}

TEST(RecordingClipJobsTest, RetainedSealCapacityIsRejectedBeforeQueueAdmission) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    auto request = MissingSourceRequest(1000000000);
    request.closed_segments.reserve(2 * 1024 * 1024 / sizeof(engine::RecordingSegmentSeal) + 1);
    const auto rejected = clips.RequestPlan(std::move(request));
    ASSERT_FALSE(rejected.succeeded());
    EXPECT_EQ(rejected.error()->code, engine::EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_TRUE(clips.Snapshots().empty());
    clips.Shutdown();
}

TEST(RecordingClipJobsTest, MarkerCountIsRejectedBeforeQueueAdmission) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    auto request = MissingSourceRequest(1000000000);
    request.markers.resize(kMaxRecordingMarkers + 1);
    const auto rejected = clips.RequestPlan(std::move(request));
    ASSERT_FALSE(rejected.succeeded());
    EXPECT_EQ(rejected.error()->code, engine::EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_TRUE(clips.Snapshots().empty());
    clips.Shutdown();
}

TEST(RecordingClipJobsTest, EscapedMarkerWireBytesAreRejectedBeforeQueueAdmission) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    auto request = MissingSourceRequest(1000000000);
    request.markers.push_back({0, RecordingMarkerType::General, std::string(350000, '\x01')});
    const auto rejected = clips.RequestPlan(std::move(request));
    ASSERT_FALSE(rejected.succeeded());
    EXPECT_EQ(rejected.error()->code, engine::EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_TRUE(clips.Snapshots().empty());
    clips.Shutdown();
}

TEST(RecordingClipJobsTest, NearBudgetAsciiMarkerIsAdmitted) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    auto request = MissingSourceRequest(1000000000);
    request.markers.push_back({0, RecordingMarkerType::General, std::string(2 * 1024 * 1024 - 1024, 'x')});
    const auto accepted = clips.RequestPlan(std::move(request));
    ASSERT_TRUE(accepted.succeeded());
    EXPECT_EQ(clips.Snapshots().size(), 1u);
    clips.Shutdown();
}

TEST(RecordingClipJobsTest, ShutdownClosesAdmissionWithoutWaitingForPausedRecording) {
    RecordingArtifactRegistry registry;
    RecordingClipJobs clips(registry, {}, [] { return true; }, {});
    ASSERT_TRUE(clips.RequestPlan(MissingSourceRequest(1000000000)).succeeded());
    clips.BeginShutdown();
    const auto rejected = clips.RequestPlan(MissingSourceRequest(2000000000));
    ASSERT_FALSE(rejected.succeeded());
    EXPECT_EQ(rejected.error()->code, engine::EngineErrorCode::Cancelled);
    clips.Shutdown();
    EXPECT_EQ(clips.Snapshots().front().phase, RecordingClipPhase::Cancelled);
}

} // namespace
} // namespace exosnap
