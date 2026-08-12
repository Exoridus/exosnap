#include "LogsAdapter.h"

#include "diagnostics/LogViewPolicy.h"
#include "diagnostics/StartupTrace.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStringConverter>
#include <QTextStream>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace exosnap::quick {
namespace {

using diagnostics::AppLog;
using diagnostics::LogEntry;
using diagnostics::LogSeverityFilter;

constexpr int kSearchDebounceMs = 100;

LogSeverityFilter FilterFromInt(int value) {
    switch (value) {
    case 1:
        return LogSeverityFilter::Info;
    case 2:
        return LogSeverityFilter::Issues;
    default:
        break;
    }
    return LogSeverityFilter::All;
}

} // namespace

LogsAdapter::LogsAdapter(QObject* parent) : QObject(parent) {
    proxy_model_.setSourceModel(&source_model_);
    source_model_.attachToAppLog();

    search_debounce_.setSingleShot(true);
    search_debounce_.setInterval(kSearchDebounceMs);
    connect(&search_debounce_, &QTimer::timeout, this, [this]() { applyPendingSearch(); });

    // Counts change on every append, eviction and filter flip; the status line is
    // derived from them, so one connection per model signal keeps both in step
    // without the view ever recomputing either.
    const auto counts_changed = [this]() {
        emit countsChanged();
        updateStatus();
    };
    connect(&source_model_, &QAbstractItemModel::rowsInserted, this, counts_changed);
    connect(&source_model_, &QAbstractItemModel::rowsRemoved, this, counts_changed);
    connect(&source_model_, &QAbstractItemModel::modelReset, this, counts_changed);
    connect(&proxy_model_, &QAbstractItemModel::rowsInserted, this, counts_changed);
    connect(&proxy_model_, &QAbstractItemModel::rowsRemoved, this, counts_changed);
    connect(&proxy_model_, &QAbstractItemModel::modelReset, this, counts_changed);

    refreshStartupTrace();
    updateStatus();
}

QAbstractItemModel* LogsAdapter::model() noexcept {
    return &proxy_model_;
}

int LogsAdapter::severityFilter() const noexcept {
    return static_cast<int>(proxy_model_.severityFilter());
}

void LogsAdapter::setSeverityFilter(int filter) {
    const LogSeverityFilter resolved = FilterFromInt(filter);
    if (proxy_model_.severityFilter() == resolved)
        return;
    proxy_model_.setSeverityFilter(resolved);
    emit filtersChanged();
    emit countsChanged();
    updateStatus();
}

QString LogsAdapter::searchQuery() const {
    return proxy_model_.searchQuery();
}

void LogsAdapter::setSearchQuery(const QString& query) {
    const QString trimmed = query.trimmed();
    if (pending_search_query_ == trimmed && proxy_model_.searchQuery() == trimmed)
        return;
    pending_search_query_ = trimmed;
    // Debounced: a keystroke must not re-filter (and re-lay-out) the whole list.
    search_debounce_.start();
}

bool LogsAdapter::autoScroll() const noexcept {
    return auto_scroll_;
}

void LogsAdapter::setAutoScroll(bool enabled) {
    if (auto_scroll_ == enabled)
        return;
    auto_scroll_ = enabled;
    emit autoScrollChanged();
}

int LogsAdapter::totalCount() const {
    return source_model_.rowCount();
}

int LogsAdapter::visibleCount() const {
    return proxy_model_.rowCount();
}

const QString& LogsAdapter::statusText() const noexcept {
    return status_text_;
}

bool LogsAdapter::canCopy() const {
    return proxy_model_.rowCount() > 0;
}

bool LogsAdapter::canExport() const {
    return source_model_.rowCount() > 0;
}

QString LogsAdapter::logFolderPath() const {
    const QString display = diagnostics::LogFolderDisplayPath(AppLog::logFilePath());
    return display.isEmpty() ? QStringLiteral("%LOCALAPPDATA%\\ExoSnap\\logs") : display;
}

QString LogsAdapter::logFilePath() const {
    return AppLog::logFilePath();
}

const QVariantList& LogsAdapter::startupTrace() const noexcept {
    return startup_trace_;
}

QString LogsAdapter::defaultExportFileName() const {
    return QStringLiteral("exosnap-log.txt");
}

void LogsAdapter::copyVisible() {
    const QString text = diagnostics::LogEntriesToText(proxy_model_.visibleEntries());
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    updateStatus(QStringLiteral("Copied %1 visible entries.").arg(proxy_model_.rowCount()));
}

void LogsAdapter::copyRange(int first, int last) {
    const QVector<LogEntry> visible = proxy_model_.visibleEntries();
    if (visible.isEmpty())
        return;
    const int lo = std::clamp(std::min(first, last), 0, static_cast<int>(visible.size()) - 1);
    const int hi = std::clamp(std::max(first, last), 0, static_cast<int>(visible.size()) - 1);
    const QString text = diagnostics::LogEntriesToText(visible.mid(lo, hi - lo + 1));
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    updateStatus(QStringLiteral("Copied %1 selected entries.").arg(hi - lo + 1));
}

void LogsAdapter::exportToUrl(const QUrl& destination) {
    const QString path = destination.isLocalFile() ? destination.toLocalFile() : destination.toString();
    if (path.isEmpty())
        return;

    if (!synthetic_) {
        QString error;
        if (!AppLog::exportHistoryToFile(path, &error)) {
            updateStatus(QStringLiteral("Export failed: %1").arg(error));
            return;
        }
        updateStatus(QStringLiteral("Exported complete history to %1.").arg(path));
        return;
    }

    // Synthetic (visual harness) history never reached AppLog, so it is written
    // straight from the model rather than through the log file.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        updateStatus(QStringLiteral("Export failed: %1").arg(file.errorString()));
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    for (const LogEntry& entry : source_model_.entries())
        stream << AppLog::formatEntry(entry) << '\n';
    stream.flush();
    updateStatus(stream.status() == QTextStream::Ok ? QStringLiteral("Exported complete history to %1.").arg(path)
                                                    : QStringLiteral("Export failed."));
}

void LogsAdapter::openLogFolder() {
    const QString path = AppLog::logFilePath();
    if (path.isEmpty()) {
        updateStatus(QStringLiteral("No log folder is available yet."));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void LogsAdapter::clear() {
    source_model_.clear();
    updateStatus(QStringLiteral("Log history cleared."));
}

void LogsAdapter::refreshStartupTrace() {
    // Late milestones (first-paint, preview-live) land after this adapter is built,
    // so the trace is re-read whenever the page becomes visible.
    startup_trace_.clear();
    for (const auto& entry : diagnostics::StartupTrace::instance().entries()) {
        QVariantMap row;
        row.insert(QStringLiteral("label"), entry.label);
        row.insert(QStringLiteral("elapsed"), QStringLiteral("%1 ms").arg(entry.elapsed_ms));
        startup_trace_.append(row);
    }
    emit startupTraceChanged();
}

void LogsAdapter::createSupportBundle(const QUrl& destination) {
    emit createSupportBundleRequested(destination);
}

void LogsAdapter::setSyntheticEntries(QVector<LogEntry> entries) {
    synthetic_ = true;
    source_model_.setSyntheticEntries(std::move(entries));
    emit countsChanged();
    updateStatus();
}

LogEntryModel& LogsAdapter::sourceModelForTest() noexcept {
    return source_model_;
}

void LogsAdapter::applyPendingSearch() {
    if (proxy_model_.searchQuery() == pending_search_query_)
        return;
    proxy_model_.setSearchQuery(pending_search_query_);
    emit searchQueryChanged();
    emit countsChanged();
    updateStatus();
}

void LogsAdapter::updateStatus(const QString& feedback) {
    const QString text =
        diagnostics::LogStatusText(proxy_model_.rowCount(), source_model_.rowCount(), proxy_model_.severityFilter(),
                                   proxy_model_.searchQuery(), feedback);
    if (status_text_ == text)
        return;
    status_text_ = text;
    emit statusChanged();
}

} // namespace exosnap::quick
