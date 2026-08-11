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
