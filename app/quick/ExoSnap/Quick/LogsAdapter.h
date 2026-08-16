#pragma once

#include "LogEntryModel.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// Narrow QML boundary for the Logs area.
//
// The history itself is diagnostics::AppLog's bounded deque, surfaced through
// LogEntryModel and filtered by LogFilterProxyModel. The filter predicate, the
// status-line composition and the copy/export text all come from
// diagnostics::LogViewPolicy, so this adapter contains no policy of its own — only
// the wiring, the search debounce, and the two file actions.
class LogsAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("LogsAdapter is provided by the application")

    // Declared as the Qt base type: qmltyperegistrar records the concrete subclass
    // under its namespaced C++ name while moc writes it unqualified, so a concrete
    // spelling here is unresolvable for qmllint.
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT FINAL)

    Q_PROPERTY(int severityFilter READ severityFilter WRITE setSeverityFilter NOTIFY filtersChanged FINAL)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged FINAL)
    Q_PROPERTY(bool autoScroll READ autoScroll WRITE setAutoScroll NOTIFY autoScrollChanged FINAL)

    Q_PROPERTY(int totalCount READ totalCount NOTIFY countsChanged FINAL)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY countsChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool canCopy READ canCopy NOTIFY countsChanged FINAL)
    Q_PROPERTY(bool canExport READ canExport NOTIFY countsChanged FINAL)

    // CONSTANT, and truthfully so: AppLog::init() resolves the session log file
    // once per process and every later write reopens the SAME path — rotation
    // renames the backups (exosnap.log.1/.2) around it and leaves exosnap.log
    // where it is. Nothing in the product repoints it; the one seam that can
    // (setLogFilePathForTesting) exists for AppLog's own rotation tests and is
    // not reachable from the running application. These properties carried a
    // NOTIFY signal that was never emitted anywhere, which claims a dynamism the
    // C++ side does not have — a binding on it would look live and never update.
    Q_PROPERTY(QString logFolderPath READ logFolderPath CONSTANT FINAL)
    Q_PROPERTY(QString logFilePath READ logFilePath CONSTANT FINAL)
    Q_PROPERTY(QVariantList startupTrace READ startupTrace NOTIFY startupTraceChanged FINAL)
    Q_PROPERTY(QString defaultExportFileName READ defaultExportFileName CONSTANT FINAL)

  public:
    explicit LogsAdapter(QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* model() noexcept;

    [[nodiscard]] int severityFilter() const noexcept;
    void setSeverityFilter(int filter);
    [[nodiscard]] QString searchQuery() const;
    void setSearchQuery(const QString& query);
    [[nodiscard]] bool autoScroll() const noexcept;
    void setAutoScroll(bool enabled);

    [[nodiscard]] int totalCount() const;
    [[nodiscard]] int visibleCount() const;
    [[nodiscard]] const QString& statusText() const noexcept;
    [[nodiscard]] bool canCopy() const;
    [[nodiscard]] bool canExport() const;
    [[nodiscard]] QString logFolderPath() const;
    [[nodiscard]] QString logFilePath() const;
    [[nodiscard]] const QVariantList& startupTrace() const noexcept;
    [[nodiscard]] QString defaultExportFileName() const;

    Q_INVOKABLE void copyVisible();
    // Copies every visible entry whose sequence falls in the inclusive range.
    // Selection lives in the view, but it is expressed in the entries' own
    // sequence numbers rather than in row indices — the history evicts from the
    // front and the filter re-maps every row, so an index captured at click time
    // names a different entry moments later. Entries in the range that the
    // filter hides, or that have since been evicted, are simply not there; the
    // rendered text still comes from the same formatter Copy and Export use, so
    // a copied selection is byte-identical to an exported line.
    Q_INVOKABLE void copySequenceRange(qint64 first_sequence, qint64 last_sequence);
    Q_INVOKABLE void exportToUrl(const QUrl& destination);
    Q_INVOKABLE void openLogFolder();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void refreshStartupTrace();
    // Routed to the one support-bundle action the Diagnostics side owns, so both
    // entry points share a single code path.
    Q_INVOKABLE void createSupportBundle(const QUrl& destination);

    // Replaces the history with a fixed set for the visual harness.
    void setSyntheticEntries(QVector<diagnostics::LogEntry> entries);
    [[nodiscard]] LogEntryModel& sourceModelForTest() noexcept;

  signals:
    void filtersChanged();
    void searchQueryChanged();
    void autoScrollChanged();
    void countsChanged();
    void statusChanged();
    void startupTraceChanged();
    void createSupportBundleRequested(const QUrl& destination);

  private:
    void applyPendingSearch();
    void updateStatus(const QString& feedback = {});

    LogEntryModel source_model_;
    LogFilterProxyModel proxy_model_;
    QTimer search_debounce_;
    QString pending_search_query_;
    QString status_text_;
    QVariantList startup_trace_;
    bool auto_scroll_ = true;
    bool synthetic_ = false;
};

} // namespace exosnap::quick
