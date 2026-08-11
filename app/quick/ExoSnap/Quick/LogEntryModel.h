#pragma once

#include "diagnostics/AppLog.h"
#include "diagnostics/LogViewPolicy.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// One model over the ONE history that already exists.
//
// diagnostics::AppLog owns a mutex-guarded, FIFO-evicting deque of at most
// kDefaultMaxEntries entries and already coalesces delivery onto the GUI thread.
// The Widgets LogsPage kept three further copies of it (entries_,
// visible_entries_ and a QTextDocument); this model keeps exactly one, and the
// filtered view is a proxy over it rather than a fourth copy.
//
// Eviction is handled incrementally: entriesAppended carries an exact
// evicted_count, which is precisely a beginRemoveRows(0, evicted_count - 1). The
// Widgets page instead re-pulled the whole history on any eviction, which is a
// full model reset every time a busy session crosses 5000 entries.
class LogEntryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("LogEntryModel is provided by LogsAdapter")

  public:
    enum Role {
        SequenceRole = Qt::UserRole + 1,
        TimestampTextRole,
        SeverityKeyRole,
        SeverityLabelRole,
        CategoryRole,
        MessageRole,
        IsIssueRole,
    };

    explicit LogEntryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] const diagnostics::LogEntry& entryAt(int row) const;
    [[nodiscard]] const QVector<diagnostics::LogEntry>& entries() const noexcept;

    // Starts mirroring diagnostics::AppLog: seeds from its history and follows
    // entriesAppended / cleared from then on.
    void attachToAppLog();

    // Replaces the history with a fixed set and stops mirroring AppLog. Used by the
    // visual harness so a screenshot shows a deterministic log, never whatever the
    // running process happened to log a millisecond earlier.
    void setSyntheticEntries(QVector<diagnostics::LogEntry> entries);

    void clear();

    [[nodiscard]] int incrementalAppendCountForTesting() const noexcept;
    [[nodiscard]] int fullResetCountForTesting() const noexcept;

    // Test seam: drives the same slot AppLog's signal would.
    void applyAppendedForTesting(const QVector<diagnostics::LogEntry>& entries, int evicted_count);

  private:
    void onEntriesAppended(const QVector<diagnostics::LogEntry>& entries, int evicted_count);
    void onLogCleared();
    void resetTo(QVector<diagnostics::LogEntry> entries);

    QVector<diagnostics::LogEntry> entries_;
    quint64 last_sequence_seen_ = 0;
    bool synthetic_ = false;
    int incremental_append_count_ = 0;
    int full_reset_count_ = 0;
};

// Severity + substring filtering over LogEntryModel, using the shared
// diagnostics::MatchesLogFilters policy so the Widgets page and this view can never
// disagree about what "Issues" means.
class LogFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("LogFilterProxyModel is provided by LogsAdapter")

  public:
    explicit LogFilterProxyModel(QObject* parent = nullptr);

    void setSeverityFilter(diagnostics::LogSeverityFilter filter);
    [[nodiscard]] diagnostics::LogSeverityFilter severityFilter() const noexcept;

    void setSearchQuery(const QString& query);
    [[nodiscard]] const QString& searchQuery() const noexcept;

    // The visible entries, oldest first — what Copy puts on the clipboard.
    [[nodiscard]] QVector<diagnostics::LogEntry> visibleEntries() const;

  protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    diagnostics::LogSeverityFilter filter_ = diagnostics::LogSeverityFilter::All;
    QString search_query_;
};

} // namespace exosnap::quick
