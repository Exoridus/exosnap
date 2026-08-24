#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantMap>

#include "RecordingErrorAdapter.h"

// The failure path, end to end at the policy level: which results raise the
// surface, what it says, which rows it shows and what each action does.
//
// The producer under test is models::BuildRecordingFailureReport — the single
// authority both frontends read. The Widgets RecordPage builds its
// RecordingErrorModel from the same call, so a divergence here is a divergence
// there too.

namespace exosnap::quick {
namespace {

UiRecordingResult FailedResult(const wchar_t* phase, uint64_t bytes = 0) {
    UiRecordingResult result;
    result.succeeded = false;
    result.error_phase = phase;
    result.hresult_text = L"0x80004005";
    result.error_detail = L"Container::Matroska requires VideoCodec::Av1";
    result.output_file_bytes = bytes;
    result.container = exosnap::engine::Container::Matroska;
    result.video_codec = exosnap::engine::VideoCodec::Av1;
    result.audio_codec = exosnap::engine::AudioCodec::Opus;
    return result;
}

QString RowValue(const QVariantList& rows, const QString& label) {
    for (const QVariant& entry : rows) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("label")).toString() == label)
            return row.value(QStringLiteral("value")).toString();
    }
    return {};
}

bool HasRow(const QVariantList& rows, const QString& label) {
    for (const QVariant& entry : rows) {
        if (entry.toMap().value(QStringLiteral("label")).toString() == label)
            return true;
    }
    return false;
}

// ─── Policy: which results reach the surface at all ──────────────────────────

TEST(RecordingFailurePolicy, SuccessRaisesNothing) {
    UiRecordingResult result;
    result.succeeded = true;

    EXPECT_FALSE(models::BuildRecordingFailureReport(result).has_value());
}

TEST(RecordingFailurePolicy, DiskSpaceAutoStopIsLeftToItsOwnNotification) {
    // The user has already been told, with an actionable "Change folder". A
    // modal on top would report the same event and offer less.
    EXPECT_FALSE(models::BuildRecordingFailureReport(FailedResult(L"DiskSpace", 4096)).has_value());
}

TEST(RecordingFailurePolicy, PartialOutputChangesTheFraming) {
    const auto could_not_start = models::BuildRecordingFailureReport(FailedResult(L"Validate", 0));
    ASSERT_TRUE(could_not_start.has_value());
    EXPECT_EQ(could_not_start->title, QStringLiteral("Recording could not start"));

    // Bytes on disk mean the session ran and was interrupted — a different
    // story, and a different place to send the user looking.
    const auto stopped = models::BuildRecordingFailureReport(FailedResult(L"Mux", 8192));
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->title, QStringLiteral("Recording stopped unexpectedly"));
    EXPECT_TRUE(stopped->summary.contains(QStringLiteral("partial file")));
}

TEST(RecordingFailurePolicy, CodecContextUsesTheSharedLabelCanon) {
    const auto report = models::BuildRecordingFailureReport(FailedResult(L"Validate"));
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->container, QStringLiteral("MKV"));
    EXPECT_EQ(report->video_codec, QStringLiteral("AV1"));
    EXPECT_EQ(report->audio_codec, QStringLiteral("Opus"));
}

// ─── Adapter: presentation and actions ───────────────────────────────────────

class RecordingErrorAdapterTest : public ::testing::Test {
  protected:
    void present(const UiRecordingResult& result, bool can_send = true) {
        const auto report = models::BuildRecordingFailureReport(result);
        ASSERT_TRUE(report.has_value());
        adapter_.present(*report, can_send);
    }

    RecordingErrorAdapter adapter_;
};

TEST_F(RecordingErrorAdapterTest, StartsInactive) {
    EXPECT_FALSE(adapter_.active());
    EXPECT_TRUE(adapter_.detailRows().isEmpty());
}

TEST_F(RecordingErrorAdapterTest, PresentBuildsTheFactRows) {
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Validate")));

    EXPECT_TRUE(adapter_.active());
    EXPECT_EQ(RowValue(adapter_.detailRows(), QStringLiteral("PHASE")), QStringLiteral("Validate"));
    EXPECT_EQ(RowValue(adapter_.detailRows(), QStringLiteral("CODE")), QStringLiteral("0x80004005"));
    EXPECT_EQ(RowValue(adapter_.detailRows(), QStringLiteral("FORMAT")),
              QString::fromUtf8("MKV \xc2\xb7 AV1 \xc2\xb7 Opus"));
}

TEST_F(RecordingErrorAdapterTest, EngineEnumTokensAreHumanized) {
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Validate")));

    // "Container::Matroska requires VideoCodec::Av1" must never reach the user
    // in its C++ spelling.
    const QString detail = RowValue(adapter_.detailRows(), QStringLiteral("DETAIL"));
    EXPECT_EQ(detail, QStringLiteral("MKV requires AV1"));
    EXPECT_FALSE(detail.contains(QStringLiteral("::")));
}

TEST_F(RecordingErrorAdapterTest, EmptyFieldsAreOmittedRatherThanShownBlank) {
    UiRecordingResult result = FailedResult(L"Encode");
    result.hresult_text.clear();
    result.error_detail.clear();
    ASSERT_NO_FATAL_FAILURE(present(result));

    EXPECT_TRUE(HasRow(adapter_.detailRows(), QStringLiteral("PHASE")));
    EXPECT_FALSE(HasRow(adapter_.detailRows(), QStringLiteral("CODE")));
    EXPECT_FALSE(HasRow(adapter_.detailRows(), QStringLiteral("DETAIL")));
}

TEST_F(RecordingErrorAdapterTest, ASecondFailureReplacesTheFirst) {
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Validate")));
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Mux", 4096)));

    EXPECT_EQ(RowValue(adapter_.detailRows(), QStringLiteral("PHASE")), QStringLiteral("Mux"));
    EXPECT_EQ(adapter_.title(), QStringLiteral("Recording stopped unexpectedly"));
}

TEST_F(RecordingErrorAdapterTest, SendIsRefusedWhenTheBuildCannotSend) {
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Validate"), /*can_send=*/false));

    QSignalSpy spy(&adapter_, &RecordingErrorAdapter::sendReportRequested);
    adapter_.sendReport();

    EXPECT_EQ(spy.count(), 0) << "a self-build must never appear to phone home";
    EXPECT_TRUE(adapter_.active()) << "a refused action must not silently close the surface";
}

TEST_F(RecordingErrorAdapterTest, SendRoutesToTheCompositionRootAndCloses) {
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Validate")));

    QSignalSpy spy(&adapter_, &RecordingErrorAdapter::sendReportRequested);
    adapter_.sendReport();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(adapter_.active());
    // The report the root reads must still describe the failure that was sent.
    EXPECT_EQ(adapter_.report().phase, QStringLiteral("Validate"));
}

TEST_F(RecordingErrorAdapterTest, OpenLogsRoutesAndCloses) {
    ASSERT_NO_FATAL_FAILURE(present(FailedResult(L"Validate")));

    QSignalSpy spy(&adapter_, &RecordingErrorAdapter::openLogsRequested);
    adapter_.openLogs();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(adapter_.active());
}

TEST_F(RecordingErrorAdapterTest, ActionsOnAnInactiveSurfaceDoNothing) {
    QSignalSpy send(&adapter_, &RecordingErrorAdapter::sendReportRequested);
    QSignalSpy logs(&adapter_, &RecordingErrorAdapter::openLogsRequested);

    adapter_.sendReport();
    adapter_.openLogs();
    adapter_.dismiss();

    EXPECT_EQ(send.count(), 0);
    EXPECT_EQ(logs.count(), 0);
}

} // namespace
} // namespace exosnap::quick
