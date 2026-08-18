// Logs presentation policy extracted out of the Qt Widgets LogsPage: the filter
// predicate, the status-line composition, the copy/export formatter and the
// footer's elided folder path.

#include "diagnostics/LogViewPolicy.h"

#include <QDateTime>

#include <gtest/gtest.h>

using namespace exosnap::diagnostics;

namespace {

LogEntry MakeEntry(LogSeverity severity, const char* category, const char* message) {
    LogEntry entry;
    entry.sequence = 1;
    entry.timestamp = QDateTime(QDate(2026, 6, 8), QTime(14, 22, 31, 123));
    entry.severity = severity;
    entry.category = QString::fromUtf8(category);
    entry.message = QString::fromUtf8(message);
    return entry;
}

} // namespace

TEST(LogViewPolicy, IssueSeverityIsWarningAndAbove) {
    EXPECT_FALSE(IsIssueSeverity(LogSeverity::Debug));
    EXPECT_FALSE(IsIssueSeverity(LogSeverity::Info));
    EXPECT_TRUE(IsIssueSeverity(LogSeverity::Warning));
    EXPECT_TRUE(IsIssueSeverity(LogSeverity::Error));
    // Critical is ranked above Error, so an Issues filter must still show it.
    EXPECT_TRUE(IsIssueSeverity(LogSeverity::Critical));
}

TEST(LogViewPolicy, InfoFilterIsExactNotMinimum) {
    const LogEntry warning = MakeEntry(LogSeverity::Warning, "Webcam", "device lost");
    EXPECT_FALSE(MatchesLogFilters(warning, LogSeverityFilter::Info, {}));
    EXPECT_TRUE(MatchesLogFilters(warning, LogSeverityFilter::Issues, {}));
    EXPECT_TRUE(MatchesLogFilters(warning, LogSeverityFilter::All, {}));
}

TEST(LogViewPolicy, SearchMatchesCategoryOrMessageCaseInsensitively) {
    const LogEntry entry = MakeEntry(LogSeverity::Info, "Encoder", "Bitstream output failed");
    EXPECT_TRUE(MatchesLogFilters(entry, LogSeverityFilter::All, QStringLiteral("encoder")));
    EXPECT_TRUE(MatchesLogFilters(entry, LogSeverityFilter::All, QStringLiteral("BITSTREAM")));
    EXPECT_FALSE(MatchesLogFilters(entry, LogSeverityFilter::All, QStringLiteral("webcam")));
}

TEST(LogViewPolicy, SearchAndSeverityCombine) {
    const LogEntry entry = MakeEntry(LogSeverity::Debug, "Preview", "crop resolved");
    EXPECT_FALSE(MatchesLogFilters(entry, LogSeverityFilter::Issues, QStringLiteral("crop")));
    EXPECT_TRUE(MatchesLogFilters(entry, LogSeverityFilter::All, QStringLiteral("crop")));
}

TEST(LogViewPolicy, StatusTextNamesCountsFilterAndSearch) {
    const QString text = LogStatusText(3, 42, LogSeverityFilter::Issues, QStringLiteral("nvenc"));
    EXPECT_TRUE(text.contains(QStringLiteral("Showing 3 of 42 entries")));
    EXPECT_TRUE(text.contains(QStringLiteral("Issues")));
    EXPECT_TRUE(text.contains(QStringLiteral("search \"nvenc\"")));
}

TEST(LogViewPolicy, StatusTextSaysNoSearchWhenEmptyAndAppendsFeedback) {
    const QString text =
        LogStatusText(42, 42, LogSeverityFilter::All, {}, QStringLiteral("Copied 42 visible entries."));
    EXPECT_TRUE(text.contains(QStringLiteral("no search")));
    EXPECT_TRUE(text.endsWith(QStringLiteral("Copied 42 visible entries.")));
}

TEST(LogViewPolicy, EntriesToTextJoinsFormattedLines) {
    const QVector<LogEntry> entries = {MakeEntry(LogSeverity::Info, "Record", "started"),
                                       MakeEntry(LogSeverity::Error, "Encoder", "failed")};
    const QString text = LogEntriesToText(entries);
    EXPECT_EQ(text.count(QLatin1Char('\n')), 1);
    EXPECT_TRUE(text.contains(QStringLiteral("started")));
    EXPECT_TRUE(text.contains(QStringLiteral("failed")));
}

TEST(LogViewPolicy, EntriesToTextIsEmptyForNoEntries) {
    EXPECT_TRUE(LogEntriesToText({}).isEmpty());
}

// The footer must not leak a full user path into a screenshot: only the parent
// directory and file name are shown, with the full path left to the tooltip.
TEST(LogViewPolicy, FolderDisplayPathKeepsOnlyParentAndFileName) {
    EXPECT_EQ(LogFolderDisplayPath(QStringLiteral("C:/Users/Someone/AppData/Local/ExoSnap/logs/exosnap.log")),
              QStringLiteral("logs/exosnap.log"));
    EXPECT_TRUE(LogFolderDisplayPath({}).isEmpty());
}

// ── Sequence-range selection (QCR-404) ──────────────────────────────────────
//
// The Logs view selects entries, not row positions. Everything below is the
// same selection surviving — or correctly shrinking under — the model changes a
// running session produces: appends, front eviction, and a filter that re-maps
// every visible row.

namespace {

LogEntry MakeSequenced(quint64 sequence, const char* message) {
    LogEntry entry = MakeEntry(LogSeverity::Info, "engine", message);
    entry.sequence = sequence;
    return entry;
}

QVector<LogEntry> Sequences(std::initializer_list<quint64> sequences) {
    QVector<LogEntry> entries;
    for (const quint64 sequence : sequences)
        entries.push_back(MakeSequenced(sequence, "entry"));
    return entries;
}

QVector<quint64> SequencesOf(const QVector<LogEntry>& entries) {
    QVector<quint64> result;
    for (const LogEntry& entry : entries)
        result.push_back(entry.sequence);
    return result;
}

} // namespace

TEST(LogViewPolicy, SequenceRangeSelectsTheInclusiveSpan) {
    const QVector<LogEntry> visible = Sequences({10, 11, 12, 13, 14});

    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(visible, 11, 13)), QVector<quint64>({11, 12, 13}));
}

TEST(LogViewPolicy, SequenceRangeAcceptsItsBoundsInEitherOrder) {
    const QVector<LogEntry> visible = Sequences({10, 11, 12});

    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(visible, 12, 10)), QVector<quint64>({10, 11, 12}));
}

TEST(LogViewPolicy, SequenceRangeIsUnmovedByAnAppend) {
    const QVector<LogEntry> before = Sequences({10, 11, 12});
    const QVector<LogEntry> after = Sequences({10, 11, 12, 13, 14});

    // The same two entries, whatever arrived after them. An index-based
    // selection is unmoved here too — this is the case that never broke.
    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(before, 11, 12)), SequencesOf(EntriesInSequenceRange(after, 11, 12)));
}

// The case an index-based selection got wrong: the history evicts from the
// front, so every surviving row moves up and index 1 becomes a different entry.
TEST(LogViewPolicy, SequenceRangeFollowsItsEntriesThroughFrontEviction) {
    const QVector<LogEntry> after_eviction = Sequences({12, 13, 14, 15});

    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(after_eviction, 13, 14)), QVector<quint64>({13, 14}));
}

TEST(LogViewPolicy, SequenceRangeShrinksToWhatSurvivedEviction) {
    // 10 and 11 were selected; 10 has since been evicted.
    const QVector<LogEntry> after_eviction = Sequences({11, 12, 13});

    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(after_eviction, 10, 11)), QVector<quint64>({11}));
}

// A filter change re-maps every visible row. The selection keeps exactly the
// entries that are still shown, and picks up nothing that merely moved into the
// rows it used to occupy.
TEST(LogViewPolicy, SequenceRangeKeepsOnlyWhatTheFilterStillShows) {
    const QVector<LogEntry> filtered = Sequences({11, 14});

    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(filtered, 11, 13)), QVector<quint64>({11}));
}

TEST(LogViewPolicy, SequenceRangeWhoseEntriesAreAllGoneSelectsNothing) {
    const QVector<LogEntry> visible = Sequences({20, 21, 22});

    EXPECT_TRUE(EntriesInSequenceRange(visible, 10, 12).isEmpty());
    EXPECT_TRUE(EntriesInSequenceRange({}, 10, 12).isEmpty());
}

TEST(LogViewPolicy, SingleEntrySelectionIsARangeOfOne) {
    const QVector<LogEntry> visible = Sequences({10, 11, 12});

    EXPECT_EQ(SequencesOf(EntriesInSequenceRange(visible, 11, 11)), QVector<quint64>({11}));
}
