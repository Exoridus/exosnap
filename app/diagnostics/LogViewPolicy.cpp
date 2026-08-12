#include "LogViewPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace exosnap::diagnostics {

QString LogSeverityFilterName(LogSeverityFilter filter) {
    switch (filter) {
    case LogSeverityFilter::All:
        return QStringLiteral("All");
    case LogSeverityFilter::Info:
        return QStringLiteral("Info");
    case LogSeverityFilter::Issues:
        return QStringLiteral("Issues");
    }
    return QStringLiteral("All");
}

bool IsIssueSeverity(LogSeverity severity) noexcept {
    return severity == LogSeverity::Warning || severity == LogSeverity::Error || severity == LogSeverity::Critical;
}

bool MatchesLogFilters(const LogEntry& entry, LogSeverityFilter filter, const QString& search_query) {
    switch (filter) {
    case LogSeverityFilter::All:
        break;
    case LogSeverityFilter::Info:
        if (entry.severity != LogSeverity::Info)
            return false;
        break;
    case LogSeverityFilter::Issues:
        if (!IsIssueSeverity(entry.severity))
            return false;
        break;
    }

    if (search_query.isEmpty())
        return true;

    return entry.message.contains(search_query, Qt::CaseInsensitive) ||
           entry.category.contains(search_query, Qt::CaseInsensitive);
}

QString LogEntriesToText(const QVector<LogEntry>& entries) {
    QStringList lines;
    lines.reserve(entries.size());
    for (const LogEntry& entry : entries)
        lines.push_back(AppLog::formatEntry(entry));
    return lines.join(QStringLiteral("\n"));
}

QString LogStatusText(int visible_count, int total_count, LogSeverityFilter filter, const QString& search_query,
                      const QString& feedback) {
    const QString search_part =
        search_query.isEmpty() ? QStringLiteral("no search") : QStringLiteral("search \"%1\"").arg(search_query);
    QString text = QStringLiteral("Showing %1 of %2 entries \xc2\xb7 %3 \xc2\xb7 %4")
                       .arg(visible_count)
                       .arg(total_count)
                       .arg(LogSeverityFilterName(filter), search_part);
    if (!feedback.isEmpty())
        text += QStringLiteral(" \xc2\xb7 %1").arg(feedback);
    return text;
}

QString LogFolderDisplayPath(const QString& log_file_path) {
    if (log_file_path.isEmpty())
        return {};
    const QFileInfo info(log_file_path);
    return info.dir().dirName() + QLatin1Char('/') + info.fileName();
}

} // namespace exosnap::diagnostics
