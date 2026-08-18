#include "LogEntryModel.h"

#include <algorithm>
#include <utility>

namespace exosnap::quick {
namespace {

using diagnostics::AppLog;
using diagnostics::LogEntry;

const LogEntry& FallbackEntry() {
    static const LogEntry entry{};
    return entry;
}

} // namespace

LogEntryModel::LogEntryModel(QObject* parent) : QAbstractListModel(parent) {
}

int LogEntryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant LogEntryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size())
        return {};
    const LogEntry& entry = entries_.at(index.row());
    switch (role) {
    case SequenceRole:
        return QVariant::fromValue(entry.sequence);
    case TimestampTextRole:
        return entry.timestamp.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"));
    case SeverityKeyRole:
        return AppLog::severityKey(entry.severity);
    case SeverityLabelRole:
        return AppLog::severityLabel(entry.severity);
    case CategoryRole:
        return entry.category.trimmed();
    case MessageRole:
        return entry.message.trimmed();
    case IsIssueRole:
        return diagnostics::IsIssueSeverity(entry.severity);
    default:
        break;
    }
    return {};
}

QHash<int, QByteArray> LogEntryModel::roleNames() const {
    return {
        {SequenceRole, "sequence"},       {TimestampTextRole, "timestampText"},
        {SeverityKeyRole, "severityKey"}, {SeverityLabelRole, "severityLabel"},
        {CategoryRole, "category"},       {MessageRole, "message"},
        {IsIssueRole, "isIssue"},
    };
}

const LogEntry& LogEntryModel::entryAt(int row) const {
    if (row < 0 || row >= entries_.size())
        return FallbackEntry();
    return entries_.at(row);
}

const QVector<LogEntry>& LogEntryModel::entries() const noexcept {
    return entries_;
}

void LogEntryModel::attachToAppLog() {
    connect(&AppLog::instance(), &AppLog::entriesAppended, this,
            [this](const QVector<LogEntry>& entries, int evicted_count) { onEntriesAppended(entries, evicted_count); });
    connect(&AppLog::instance(), &AppLog::cleared, this, [this]() { onLogCleared(); });
    resetTo(AppLog::history());
}

void LogEntryModel::setSyntheticEntries(QVector<LogEntry> entries) {
    synthetic_ = true;
    resetTo(std::move(entries));
}

void LogEntryModel::clear() {
    if (synthetic_) {
        resetTo({});
        return;
    }
    AppLog::clear();
}

int LogEntryModel::incrementalAppendCountForTesting() const noexcept {
    return incremental_append_count_;
}

int LogEntryModel::fullResetCountForTesting() const noexcept {
    return full_reset_count_;
}

void LogEntryModel::applyAppendedForTesting(const QVector<LogEntry>& entries, int evicted_count) {
    onEntriesAppended(entries, evicted_count);
}

void LogEntryModel::resetTo(QVector<LogEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    last_sequence_seen_ = 0;
    for (const LogEntry& entry : entries_)
        last_sequence_seen_ = std::max(last_sequence_seen_, entry.sequence);
    endResetModel();
    ++full_reset_count_;
}

void LogEntryModel::onEntriesAppended(const QVector<LogEntry>& entries, int evicted_count) {
    if (synthetic_ || entries.isEmpty())
        return;

    QVector<LogEntry> fresh;
    fresh.reserve(entries.size());
    for (const LogEntry& entry : entries) {
        // A sequence of 0 predates numbering; anything already seen is a redelivery.
        if (entry.sequence != 0 && entry.sequence <= last_sequence_seen_)
            continue;
        fresh.push_back(entry);
        last_sequence_seen_ = std::max(last_sequence_seen_, entry.sequence);
    }
    if (fresh.isEmpty())
        return;

    // evicted_count is exact, so eviction is a precise front removal rather than a
    // reason to re-pull the whole bounded history.
    const int evicted = std::min(evicted_count, static_cast<int>(entries_.size()));
    if (evicted > 0) {
        beginRemoveRows({}, 0, evicted - 1);
        entries_.remove(0, evicted);
        endRemoveRows();
    }

    const int first = static_cast<int>(entries_.size());
    beginInsertRows({}, first, first + static_cast<int>(fresh.size()) - 1);
    entries_.append(fresh);
    endInsertRows();
    ++incremental_append_count_;
}

void LogEntryModel::onLogCleared() {
    if (synthetic_)
        return;
    resetTo({});
}

// ── LogFilterProxyModel ─────────────────────────────────────────────────────────

LogFilterProxyModel::LogFilterProxyModel(QObject* parent) : QSortFilterProxyModel(parent) {
}

// Qt 6.11 deprecates invalidateRowsFilter() in favour of a begin/end pair, and the
// pair is not a rename: beginFilterChange() has to bracket the mutation, because it
// is what lets the proxy keep the persistent indexes across the re-filter. Only the
// rows direction is announced -- neither filter here touches columns, and claiming
// Both would invalidate a column mapping that never changed.
void LogFilterProxyModel::setSeverityFilter(diagnostics::LogSeverityFilter filter) {
    if (filter_ == filter)
        return;
    beginFilterChange();
    filter_ = filter;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

diagnostics::LogSeverityFilter LogFilterProxyModel::severityFilter() const noexcept {
    return filter_;
}

void LogFilterProxyModel::setSearchQuery(const QString& query) {
    if (search_query_ == query)
        return;
    beginFilterChange();
    search_query_ = query;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

const QString& LogFilterProxyModel::searchQuery() const noexcept {
    return search_query_;
}

QVector<LogEntry> LogFilterProxyModel::visibleEntries() const {
    const auto* source = qobject_cast<const LogEntryModel*>(sourceModel());
    QVector<LogEntry> visible;
    if (source == nullptr)
        return visible;
    const int count = rowCount();
    visible.reserve(count);
    for (int row = 0; row < count; ++row)
        visible.push_back(source->entryAt(mapToSource(index(row, 0)).row()));
    return visible;
}

bool LogFilterProxyModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    if (source_parent.isValid())
        return false;
    const auto* source = qobject_cast<const LogEntryModel*>(sourceModel());
    if (source == nullptr)
        return true;
    return diagnostics::MatchesLogFilters(source->entryAt(source_row), filter_, search_query_);
}

} // namespace exosnap::quick
