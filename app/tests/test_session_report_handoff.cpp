// The handoff from the session ledger into the on-disk session report.
//
// The ledger is frozen by the Qt main thread when it processes the terminal
// diagnostics snapshot; the report is gathered on the recording thread. Only the
// order in which the two reach the main thread's queue decides whether the report
// carries the ledger, and nothing about that order is visible in either half on
// its own -- which is what these tests pin. The engine is not involved: the
// coordinator's own posting path is driven directly.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <vector>

#include "diagnostics/AppLog.h"
#include "diagnostics/SessionLedger.h"
#include "services/RecordingCoordinator.h"

namespace exosnap {
namespace {

diagnostics::LedgerEntry JudderEntry() {
    diagnostics::LedgerEntry entry;
    entry.id = "rec.001";
    entry.title = "Frame pacing";
    entry.summary = "Presents arrived unevenly";
    entry.worst = 9.0;
    entry.worst_text = "9.0 ms";
    entry.budget = 8.0;
    entry.unit = "ms";
    entry.count = 1;
    entry.first_seen_s = 2.0;
    entry.last_seen_s = 5.0;
    entry.total_active_s = 3.0;
    entry.occurrences.push_back({2.0, 5.0, 9.0});
    return entry;
}

exosnap::engine::RecordingDiagnosticsSnapshot TerminalSnapshot() {
    exosnap::engine::RecordingDiagnosticsSnapshot snapshot;
    snapshot.lifecycle = exosnap::engine::DiagnosticsLifecycle::Completed;
    snapshot.valid = true;
    snapshot.elapsed_seconds = 5.0;
    return snapshot;
}

UiRecordingResult CompletedResult() {
    UiRecordingResult result;
    result.succeeded = true;
    result.elapsed_seconds = 5.0;
    result.output_path = L"C:/somewhere/ExoSnap_test.mkv";
    result.output_file_bytes = 4096;
    return result;
}

// Reads the single report the coordinator wrote, or a null document when it wrote
// none.
QJsonObject ReadOnlyReport(const QString& reports_dir) {
    const QStringList names = QDir(reports_dir).entryList({QStringLiteral("session-*.json")}, QDir::Files);
    if (names.size() != 1)
        return {};
    QFile file(reports_dir + QLatin1Char('/') + names.first());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

// The production wiring in one line: the diagnostics side freezes the ledger on
// the main thread when it sees the terminal snapshot, and pushes it into the sink.
void FreezeLedgerOnTerminalSnapshot(RecordingCoordinator& coordinator, std::vector<diagnostics::LedgerEntry> ledger) {
    coordinator.SetDiagnosticsCallback(
        [sink = coordinator.FrozenLedgerSink(), ledger](const exosnap::engine::RecordingDiagnosticsSnapshot& s) {
            if (s.lifecycle == exosnap::engine::DiagnosticsLifecycle::Completed)
                sink->Set(ledger);
        });
}

class SessionReportHandoffTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(temp_.isValid());
        // The report goes into a "reports" directory beside the log file.
        diagnostics::AppLog::setLogFilePathForTesting(temp_.path() + QStringLiteral("/exosnap.log"));
        reports_dir_ = temp_.path() + QStringLiteral("/reports");
    }

    QTemporaryDir temp_;
    QString reports_dir_;
};

// The defect this pins: the terminal snapshot is delivered to the main thread
// through a queued call, so a report written inline on the recording thread is
// written before the freeze and carries nothing. The report must wait its turn in
// the same queue.
TEST_F(SessionReportHandoffTest, TheReportCarriesTheLedgerFrozenWhileTheResultWasInFlight) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    coordinator.BeginReportSessionForTest(QStringLiteral("rec-handoff"));
    FreezeLedgerOnTerminalSnapshot(coordinator, {JudderEntry()});

    // Exactly the order the stop path produces: the terminal snapshot is posted,
    // then the result -- both from the recording thread, neither of them drained
    // yet.
    coordinator.PostDiagnosticsForTest(TerminalSnapshot());
    coordinator.PostResultForTest(CompletedResult());
    EXPECT_FALSE(QDir(reports_dir_).exists()) << "the report must not be written before the freeze is processed";

    QCoreApplication::processEvents();

    const QJsonObject report = ReadOnlyReport(reports_dir_);
    ASSERT_FALSE(report.isEmpty());
    ASSERT_TRUE(report.contains(QStringLiteral("ledger"))) << "the frozen ledger never reached the report";
    const QJsonArray entries = report.value(QStringLiteral("ledger")).toArray();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("rec.001"));
}

// The other direction, so the assertion above cannot pass by writing a ledger key
// unconditionally: a recording that measured nothing says nothing.
TEST_F(SessionReportHandoffTest, ACleanRecordingStillWritesAReportWithoutALedger) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    coordinator.BeginReportSessionForTest(QStringLiteral("rec-clean"));
    FreezeLedgerOnTerminalSnapshot(coordinator, {});

    coordinator.PostDiagnosticsForTest(TerminalSnapshot());
    coordinator.PostResultForTest(CompletedResult());
    QCoreApplication::processEvents();

    const QJsonObject report = ReadOnlyReport(reports_dir_);
    ASSERT_FALSE(report.isEmpty());
    EXPECT_FALSE(report.contains(QStringLiteral("ledger")));
}

// A ledger frozen for the previous recording must not be reported as this one's.
TEST_F(SessionReportHandoffTest, TheNextRecordingDoesNotInheritTheLedger) {
    int argc = 0;
    QCoreApplication app(argc, nullptr);

    RecordingCoordinator coordinator;
    coordinator.FrozenLedgerSink()->Set({JudderEntry()});
    coordinator.BeginReportSessionForTest(QStringLiteral("rec-second"));

    coordinator.PostResultForTest(CompletedResult());
    QCoreApplication::processEvents();

    const QJsonObject report = ReadOnlyReport(reports_dir_);
    ASSERT_FALSE(report.isEmpty());
    EXPECT_FALSE(report.contains(QStringLiteral("ledger")));
}

} // namespace
} // namespace exosnap
