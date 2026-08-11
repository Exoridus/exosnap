// LogEntryModel layers directly over AppLog's bounded deque. These tests pin the
// two things that make it cheaper than the Widgets page it replaces: an append is
// an incremental insert (not a reset), and an eviction is an exact front removal
// derived from entriesAppended's evicted_count (not a re-pull of the history).

#include "LogEntryModel.h"

#include <QCoreApplication>
#include <QDateTime>

#include <gtest/gtest.h>

#include <vector>

using exosnap::diagnostics::LogEntry;
using exosnap::diagnostics::LogSeverity;
using exosnap::diagnostics::LogSeverityFilter;
using exosnap::quick::LogEntryModel;
using exosnap::quick::LogFilterProxyModel;

namespace {

QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "log_entry_model_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

LogEntry MakeEntry(quint64 sequence, LogSeverity severity, const char* category, const char* message) {
    LogEntry entry;
    entry.sequence = sequence;
    entry.timestamp = QDateTime(QDate(2026, 6, 8), QTime(14, 22, 31, 123));
    entry.severity = severity;
    entry.category = QString::fromUtf8(category);
    entry.message = QString::fromUtf8(message);
    return entry;
}

QVector<LogEntry> MakeEntries(int count, quint64 first_sequence = 1) {
    QVector<LogEntry> entries;
    for (int i = 0; i < count; ++i)
        entries.push_back(MakeEntry(first_sequence + static_cast<quint64>(i), LogSeverity::Info, "Record", "entry"));
    return entries;
}

QVariant RoleAt(const LogEntryModel& model, int row, int role) {
    return model.data(model.index(row, 0), role);
}

// Records rowsRemoved(parent, first, last) without pulling in Qt Test.
struct RemovalRecorder {
    int count = 0;
    int first = -1;
    int last = -1;

    explicit RemovalRecorder(LogEntryModel& model) {
        QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                         [this](const QModelIndex&, int from, int to) {
                             ++count;
                             first = from;
                             last = to;
                         });
    }
};

} // namespace

TEST(LogEntryModelTest, RolesRenderTheEntryColumns) {
    EnsureApplication();
    LogEntryModel model;
    model.setSyntheticEntries({MakeEntry(7, LogSeverity::Warning, "  Webcam  ", "  device lost  ")});

    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(RoleAt(model, 0, LogEntryModel::SequenceRole).toULongLong(), 7U);
    EXPECT_EQ(RoleAt(model, 0, LogEntryModel::TimestampTextRole).toString(), QStringLiteral("2026-06-08T14:22:31.123"));
    EXPECT_EQ(RoleAt(model, 0, LogEntryModel::CategoryRole).toString(), QStringLiteral("Webcam"));
    EXPECT_EQ(RoleAt(model, 0, LogEntryModel::MessageRole).toString(), QStringLiteral("device lost"));
    EXPECT_TRUE(RoleAt(model, 0, LogEntryModel::IsIssueRole).toBool());
    EXPECT_FALSE(RoleAt(model, 0, LogEntryModel::SeverityLabelRole).toString().isEmpty());
}

TEST(LogEntryModelTest, RoleNamesCoverTheDelegateContract) {
    EnsureApplication();
    LogEntryModel model;
    const auto names = model.roleNames();
    for (const char* expected :
         {"sequence", "timestampText", "severityKey", "severityLabel", "category", "message", "isIssue"}) {
        EXPECT_TRUE(names.values().contains(QByteArray(expected))) << expected;
    }
}

TEST(LogEntryModelTest, AppendIsAnIncrementalInsertNotAReset) {
    EnsureApplication();
    LogEntryModel model;
    int resets = 0;
    int inserts = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&resets]() { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                     [&inserts](const QModelIndex&, int, int) { ++inserts; });

    model.applyAppendedForTesting(MakeEntries(3, 1), 0);
    EXPECT_EQ(model.rowCount(), 3);
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(inserts, 1);
    EXPECT_EQ(model.incrementalAppendCountForTesting(), 1);
}

// The Widgets page re-pulled the entire history whenever anything was evicted.
// evicted_count is exact, so eviction is a precise front removal instead.
TEST(LogEntryModelTest, EvictionRemovesExactlyTheEvictedFrontRows) {
    EnsureApplication();
    LogEntryModel model;
    model.applyAppendedForTesting(MakeEntries(5, 1), 0);
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&resets]() { ++resets; });
    RemovalRecorder removals(model);

    model.applyAppendedForTesting(MakeEntries(2, 6), 2);

    EXPECT_EQ(resets, 0);
    ASSERT_EQ(removals.count, 1);
    EXPECT_EQ(removals.first, 0);
    EXPECT_EQ(removals.last, 1);
    ASSERT_EQ(model.rowCount(), 5);
    EXPECT_EQ(RoleAt(model, 0, LogEntryModel::SequenceRole).toULongLong(), 3U);
    EXPECT_EQ(RoleAt(model, 4, LogEntryModel::SequenceRole).toULongLong(), 7U);
}

TEST(LogEntryModelTest, RedeliveredSequencesAreIgnored) {
    EnsureApplication();
    LogEntryModel model;
    model.applyAppendedForTesting(MakeEntries(3, 1), 0);
    model.applyAppendedForTesting(MakeEntries(3, 1), 0);
    EXPECT_EQ(model.rowCount(), 3);
}

TEST(LogEntryModelTest, EvictionNeverRemovesMoreRowsThanExist) {
    EnsureApplication();
    LogEntryModel model;
    model.applyAppendedForTesting(MakeEntries(2, 1), 0);
    model.applyAppendedForTesting(MakeEntries(1, 3), 99);
    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(RoleAt(model, 0, LogEntryModel::SequenceRole).toULongLong(), 3U);
}

TEST(LogEntryModelTest, SyntheticHistoryStopsFollowingAppends) {
    EnsureApplication();
    LogEntryModel model;
    model.setSyntheticEntries(MakeEntries(2, 1));
    model.applyAppendedForTesting(MakeEntries(3, 10), 0);
    EXPECT_EQ(model.rowCount(), 2);
}

// ── Proxy ───────────────────────────────────────────────────────────────────────

TEST(LogFilterProxyModelTest, SeverityFilterUsesTheSharedPolicy) {
    EnsureApplication();
    LogEntryModel source;
    source.setSyntheticEntries({MakeEntry(1, LogSeverity::Debug, "Preview", "crop"),
                                MakeEntry(2, LogSeverity::Info, "Record", "started"),
                                MakeEntry(3, LogSeverity::Error, "Encoder", "failed")});
    LogFilterProxyModel proxy;
    proxy.setSourceModel(&source);
    EXPECT_EQ(proxy.rowCount(), 3);

    proxy.setSeverityFilter(LogSeverityFilter::Issues);
    ASSERT_EQ(proxy.rowCount(), 1);
    EXPECT_EQ(proxy.visibleEntries().at(0).message, QStringLiteral("failed"));

    proxy.setSeverityFilter(LogSeverityFilter::Info);
    ASSERT_EQ(proxy.rowCount(), 1);
    EXPECT_EQ(proxy.visibleEntries().at(0).message, QStringLiteral("started"));
}

TEST(LogFilterProxyModelTest, SearchNarrowsWithinTheSeverityFilter) {
    EnsureApplication();
    LogEntryModel source;
    source.setSyntheticEntries({MakeEntry(1, LogSeverity::Warning, "Webcam", "device lost"),
                                MakeEntry(2, LogSeverity::Error, "Encoder", "device removed")});
    LogFilterProxyModel proxy;
    proxy.setSourceModel(&source);
    proxy.setSeverityFilter(LogSeverityFilter::Issues);
    proxy.setSearchQuery(QStringLiteral("webcam"));
    ASSERT_EQ(proxy.rowCount(), 1);
    EXPECT_EQ(proxy.visibleEntries().at(0).category, QStringLiteral("Webcam"));
}

TEST(LogFilterProxyModelTest, VisibleEntriesKeepOldestFirstOrder) {
    EnsureApplication();
    LogEntryModel source;
    source.setSyntheticEntries(MakeEntries(4, 1));
    LogFilterProxyModel proxy;
    proxy.setSourceModel(&source);
    const auto visible = proxy.visibleEntries();
    ASSERT_EQ(visible.size(), 4);
    EXPECT_EQ(visible.front().sequence, 1U);
    EXPECT_EQ(visible.back().sequence, 4U);
}
