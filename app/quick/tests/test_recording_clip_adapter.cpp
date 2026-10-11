#include "RecordingClipAdapter.h"

#include <gtest/gtest.h>

#include <QString>
#include <QVariantMap>

#include <cstdint>

namespace exosnap::quick {
namespace {

TEST(RecordingClipAdapterTest, MetadataBudgetFailureDoesNotClaimQueueIsFull) {
    RecordingClipAdapter adapter;
    RecordingClipJobSnapshot failed;
    failed.job_id = engine::NewId<engine::JobId>();
    failed.generation = 1;
    failed.phase = RecordingClipPhase::Failed;
    failed.error = engine::EngineError{};
    failed.error->code = engine::EngineErrorCode::ResourceBudgetExceeded;
    adapter.setJobs({failed});
    const auto row = adapter.jobs().front().toMap();
    EXPECT_EQ(row.value(QStringLiteral("errorSummary")).toString(),
              RecordingClipAdapter::tr("The clip could not be saved."));
}

TEST(RecordingClipAdapterTest, PositiveDurationRequiresBackendAvailability) {
    RecordingClipAdapter adapter;
    int requests = 0;
    int64_t requested_seconds = 0;
    QObject::connect(&adapter, &RecordingClipAdapter::planRequested, [&](int64_t seconds) {
        ++requests;
        requested_seconds = seconds;
    });
    adapter.requestPlan(QStringLiteral("30"));
    EXPECT_EQ(requests, 0);
    adapter.setAvailability(true, {});
    for (const auto& invalid :
         {QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("1.5"), QStringLiteral("9223372036854775808")})
        adapter.requestPlan(invalid);
    EXPECT_EQ(requests, 0);
    adapter.requestPlan(QStringLiteral("1800"));
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(requested_seconds, 1800);
}

TEST(RecordingClipAdapterTest, ReviewUsesExactStringTokensAndKeepsActualRangeDistinct) {
    RecordingClipAdapter adapter;
    RecordingClipJobSnapshot review;
    review.job_id = engine::NewId<engine::JobId>();
    review.generation = 9'007'199'254'740'993ULL;
    review.phase = RecordingClipPhase::Review;
    review.requested_begin_r_ns = 30'000'000'000;
    review.requested_end_r_ns = 60'000'000'000;
    review.requested_duration_seconds = 300;
    review.actual_begin_r_ns = 28'000'000'000;
    review.actual_end_r_ns = 54'000'000'000;
    review.track_names = {"Master", "Microphone"};
    adapter.setJobs({review});
    const auto row = adapter.jobs().front().toMap();
    EXPECT_EQ(row.value(QStringLiteral("requestedBegin")).toString(), QStringLiteral("00:00:30"));
    EXPECT_EQ(row.value(QStringLiteral("actualBegin")).toString(), QStringLiteral("00:00:28"));
    EXPECT_EQ(row.value(QStringLiteral("actualEnd")).toString(), QStringLiteral("00:00:54"));
    EXPECT_EQ(row.value(QStringLiteral("trackNames")).toStringList().size(), 2);
    EXPECT_FALSE(row.contains(QStringLiteral("progressPercent")));
    int confirms = 0;
    QObject::connect(&adapter, &RecordingClipAdapter::confirmRequested, [&](engine::JobId id, uint64_t generation) {
        EXPECT_EQ(id, review.job_id);
        EXPECT_EQ(generation, review.generation);
        ++confirms;
    });
    adapter.confirmPlan(row.value(QStringLiteral("jobId")).toString(),
                        row.value(QStringLiteral("generation")).toString());
    EXPECT_EQ(confirms, 1);
    EXPECT_EQ(adapter.preferredDurationSeconds(), QStringLiteral("30"));
    adapter.acceptConfirmedDuration(review.requested_duration_seconds);
    EXPECT_EQ(adapter.preferredDurationSeconds(), QStringLiteral("300"));
    adapter.confirmPlan(row.value(QStringLiteral("jobId")).toString(), QStringLiteral("9007199254740992"));
    EXPECT_EQ(confirms, 1);
}

TEST(RecordingClipAdapterTest, RejectedConfirmationDoesNotChangePreferredDuration) {
    RecordingClipAdapter adapter;
    RecordingClipJobSnapshot review;
    review.job_id = engine::NewId<engine::JobId>();
    review.generation = 1;
    review.phase = RecordingClipPhase::Review;
    review.actual_begin_r_ns = 1;
    review.actual_end_r_ns = 2;
    review.requested_duration_seconds = 1800;
    adapter.setJobs({review});
    int changes = 0;
    QObject::connect(&adapter, &RecordingClipAdapter::preferredDurationChanged, [&] { ++changes; });
    const auto row = adapter.jobs().front().toMap();
    adapter.confirmPlan(row.value(QStringLiteral("jobId")).toString(),
                        row.value(QStringLiteral("generation")).toString());
    EXPECT_EQ(adapter.preferredDurationSeconds(), QStringLiteral("30"));
    EXPECT_EQ(changes, 0);
    adapter.acceptConfirmedDuration(review.requested_duration_seconds);
    EXPECT_EQ(adapter.preferredDurationSeconds(), QStringLiteral("1800"));
    EXPECT_EQ(changes, 1);
    adapter.acceptConfirmedDuration(review.requested_duration_seconds);
    EXPECT_EQ(changes, 1);
}

TEST(RecordingClipAdapterTest, LateGenerationCannotRegressCompletedJobOrOpenAnUnverifiedAsset) {
    RecordingClipAdapter adapter;
    RecordingClipJobSnapshot completed;
    completed.job_id = engine::NewId<engine::JobId>();
    completed.generation = 3;
    completed.phase = RecordingClipPhase::Completed;
    completed.asset_id = engine::NewId<engine::MediaId>();
    completed.completed = CompletedRecording{};
    completed.completed->succeeded = true;
    adapter.setJobs({completed});
    auto late = completed;
    late.generation = 2;
    late.phase = RecordingClipPhase::Failed;
    adapter.setJobs({late});
    const auto row = adapter.jobs().front().toMap();
    EXPECT_TRUE(row.value(QStringLiteral("canOpen")).toBool());
    EXPECT_EQ(row.value(QStringLiteral("generation")).toString(), QStringLiteral("3"));
    int opens = 0;
    QObject::connect(&adapter, &RecordingClipAdapter::openCompletedRequested, [&](engine::MediaId id) {
        EXPECT_EQ(id, *completed.asset_id);
        ++opens;
    });
    adapter.openCompleted(row.value(QStringLiteral("assetId")).toString());
    EXPECT_EQ(opens, 1);
    completed.completed->succeeded = false;
    adapter.setJobs({completed});
    adapter.openCompleted(row.value(QStringLiteral("assetId")).toString());
    EXPECT_EQ(opens, 1);
}

TEST(RecordingClipAdapterTest, CancelRequiresCurrentGenerationAndAnActiveJob) {
    RecordingClipAdapter adapter;
    RecordingClipJobSnapshot queued;
    queued.job_id = engine::NewId<engine::JobId>();
    queued.generation = 5;
    queued.phase = RecordingClipPhase::Queued;
    adapter.setJobs({queued});
    const auto job_id = QString::fromStdString(engine::ToUuid(queued.job_id));
    int cancellations = 0;
    QObject::connect(&adapter, &RecordingClipAdapter::cancelRequested, [&](engine::JobId id, uint64_t generation) {
        EXPECT_EQ(id, queued.job_id);
        EXPECT_EQ(generation, queued.generation);
        ++cancellations;
    });
    adapter.cancelJob(job_id, QStringLiteral("4"));
    EXPECT_EQ(cancellations, 0);
    adapter.cancelJob(job_id, QStringLiteral("5"));
    EXPECT_EQ(cancellations, 1);
    queued.phase = RecordingClipPhase::Cancelled;
    adapter.setJobs({queued});
    adapter.cancelJob(job_id, QStringLiteral("5"));
    EXPECT_EQ(cancellations, 1);
}

} // namespace
} // namespace exosnap::quick
