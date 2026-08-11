#pragma once

#include "AppLog.h"

#include <QString>
#include <QVector>

// Presentation policy for the Logs surface, extracted from the Qt Widgets LogsPage.
//
// Pure functions over LogEntry — no widget, no model, no view. Both the Widgets
// page and the Quick LogEntryModel/LogsAdapter can share the same answers for
// "does this entry match", "what does the status line read", and "what gets copied".
namespace exosnap::diagnostics {

enum class LogSeverityFilter {
    All = 0,
    Info = 1,
    Issues = 2,
};

[[nodiscard]] QString LogSeverityFilterName(LogSeverityFilter filter);

// Warning and above. Critical is ranked above Error, so an Issues filter shows it.
[[nodiscard]] bool IsIssueSeverity(LogSeverity severity) noexcept;

[[nodiscard]] bool MatchesLogFilters(const LogEntry& entry, LogSeverityFilter filter, const QString& search_query);

[[nodiscard]] QString LogEntriesToText(const QVector<LogEntry>& entries);

// "Showing 12 of 480 entries · All · no search[ · feedback]".
[[nodiscard]] QString LogStatusText(int visible_count, int total_count, LogSeverityFilter filter,
                                    const QString& search_query, const QString& feedback = {});

// Footer link text: parent directory + file name, so the status line never leaks a
// full user path into a screenshot. Empty when no log file exists yet.
[[nodiscard]] QString LogFolderDisplayPath(const QString& log_file_path);

} // namespace exosnap::diagnostics
