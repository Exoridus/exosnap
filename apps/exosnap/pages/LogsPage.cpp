#include "LogsPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStyle>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/SectionRuleHeader.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "../visual_tests/VisualScenario.h"
#endif

#include <algorithm>

namespace exosnap {
namespace {

using M = ui::theme::ExoSnapMetrics;
using diagnostics::AppLog;
using diagnostics::LogEntry;
using diagnostics::LogSeverity;

constexpr int kSearchDebounceMs = 100;

QString filterName(LogsPage::SeverityFilter filter) {
    switch (filter) {
    case LogsPage::SeverityFilter::All:
        return QStringLiteral("All");
    case LogsPage::SeverityFilter::Info:
        return QStringLiteral("Info");
    case LogsPage::SeverityFilter::Issues:
        return QStringLiteral("Issues");
    }
    return QStringLiteral("All");
}

QPushButton* makeFilterButton(const QString& object_name, const QString& label, QWidget* parent) {
    auto* button = new QPushButton(label, parent);
    button->setObjectName(object_name);
    button->setCheckable(true);
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("logFilterSegment", true);
    button->setProperty("qualitySegment", true);
    button->setProperty("qualitySegmentSelected", false);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return button;
}

bool isIssueSeverity(LogSeverity severity) {
    return severity == LogSeverity::Warning || severity == LogSeverity::Error;
}

QString entriesToText(const QVector<LogEntry>& entries) {
    QStringList lines;
    lines.reserve(entries.size());
    for (const LogEntry& entry : entries)
        lines.push_back(AppLog::formatEntry(entry));
    return lines.join(QStringLiteral("\n"));
}

QDateTime visualBaseTimestamp() {
    return QDateTime(QDate(2026, 6, 8), QTime(14, 22, 31, 123));
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
LogEntry visualEntry(int index, LogSeverity severity, const QString& category, const QString& message) {
    return {
        0, visualBaseTimestamp().addMSecs(index * 137), severity, category, message,
    };
}

QVector<LogEntry> visualAllLevelsEntries() {
    return {
        visualEntry(0, LogSeverity::Debug, QStringLiteral("Preview"), QStringLiteral("DXGI preview crop resolved")),
        visualEntry(1, LogSeverity::Info, QStringLiteral("Record"), QStringLiteral("Recording profile loaded")),
        visualEntry(2, LogSeverity::Warning, QStringLiteral("Webcam"),
                    QStringLiteral("Device disconnected; PiP hidden")),
        visualEntry(3, LogSeverity::Error, QStringLiteral("Encoder"),
                    QStringLiteral("AV1 encoder initialization failed")),
    };
}

QVector<LogEntry> visualSearchEntries() {
    return {
        visualEntry(0, LogSeverity::Info, QStringLiteral("Record"), QStringLiteral("Recording started")),
        visualEntry(1, LogSeverity::Debug, QStringLiteral("Webcam"), QStringLiteral("Synthetic frame delivered")),
        visualEntry(2, LogSeverity::Warning, QStringLiteral("Preview"), QStringLiteral("Webcam overlay unavailable")),
        visualEntry(3, LogSeverity::Error, QStringLiteral("Encoder"), QStringLiteral("Bitstream output failed")),
    };
}

QVector<LogEntry> visualLongMessageEntries() {
    return {visualEntry(0, LogSeverity::Warning, QStringLiteral("Output"),
                        QStringLiteral("Output folder validation returned a long recoverable warning with enough "
                                       "detail to wrap across the log surface while remaining selectable and copyable: "
                                       "C:/Users/User/Videos/ExoSnap/Very/Long/Path/That/Still/Needs/To/Be/Readable"))};
}

QVector<LogEntry> visualTruncatedEntries() {
    QVector<LogEntry> entries;
    entries.reserve(AppLog::kDefaultMaxEntries);
    const int first_sequence = 1001;
    for (int i = 0; i < AppLog::kDefaultMaxEntries; ++i) {
        const int sequence = first_sequence + i;
        const LogSeverity severity = (sequence % 11 == 0)  ? LogSeverity::Error
                                     : (sequence % 5 == 0) ? LogSeverity::Warning
                                     : (sequence % 3 == 0) ? LogSeverity::Debug
                                                           : LogSeverity::Info;
        entries.push_back(visualEntry(i, severity, QStringLiteral("Buffer"),
                                      QStringLiteral("bounded history retained entry #%1").arg(sequence)));
    }
    return entries;
}

LogsPage::SeverityFilter filterFromVisual(visual::VisualLogFilter filter) {
    switch (filter) {
    case visual::VisualLogFilter::All:
        return LogsPage::SeverityFilter::All;
    case visual::VisualLogFilter::Info:
        return LogsPage::SeverityFilter::Info;
    case visual::VisualLogFilter::Issues:
        return LogsPage::SeverityFilter::Issues;
    }
    return LogsPage::SeverityFilter::All;
}
#endif

} // namespace

LogsPage::LogsPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("logsPage"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* content = new QWidget();
    content->setMaximumWidth(1320);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceMd);

    auto* toolbar = new ui::widgets::SectionRuleHeader(QStringLiteral("APPLICATION LOG"), content);
    layout->addWidget(toolbar);

    auto* control_row = new QHBoxLayout();
    control_row->setSpacing(M::kSpaceSm);

    auto* segmented = new QWidget(content);
    segmented->setObjectName(QStringLiteral("logSeveritySegmented"));
    auto* segmented_layout = new QHBoxLayout(segmented);
    segmented_layout->setContentsMargins(3, 3, 3, 3);
    segmented_layout->setSpacing(0);

    severity_group_ = new QButtonGroup(this);
    severity_group_->setExclusive(true);
    auto* all_btn = makeFilterButton(QStringLiteral("logFilterAllBtn"), QStringLiteral("All"), segmented);
    auto* info_btn = makeFilterButton(QStringLiteral("logFilterInfoBtn"), QStringLiteral("Info"), segmented);
    auto* issues_btn = makeFilterButton(QStringLiteral("logFilterIssuesBtn"), QStringLiteral("Issues"), segmented);
    severity_group_->addButton(all_btn, static_cast<int>(SeverityFilter::All));
    severity_group_->addButton(info_btn, static_cast<int>(SeverityFilter::Info));
    severity_group_->addButton(issues_btn, static_cast<int>(SeverityFilter::Issues));
    segmented_layout->addWidget(all_btn);
    segmented_layout->addWidget(info_btn);
    segmented_layout->addWidget(issues_btn);
    control_row->addWidget(segmented, 0);

    search_edit_ = new QLineEdit(content);
    search_edit_->setObjectName(QStringLiteral("logSearchEdit"));
    search_edit_->setPlaceholderText(QStringLiteral("Search category or message"));
    search_edit_->setClearButtonEnabled(true);
    control_row->addWidget(search_edit_, 1);

    auto_scroll_check_ = new QCheckBox(QStringLiteral("Auto-scroll"), content);
    auto_scroll_check_->setObjectName(QStringLiteral("logAutoScrollToggle"));
    auto_scroll_check_->setChecked(true);
    control_row->addWidget(auto_scroll_check_, 0);

    refresh_btn_ = new QPushButton(QStringLiteral("Refresh"), content);
    refresh_btn_->setObjectName(QStringLiteral("logRefreshBtn"));
    refresh_btn_->setProperty("role", "ghost");
    clear_btn_ = new QPushButton(QStringLiteral("Clear"), content);
    clear_btn_->setObjectName(QStringLiteral("logClearBtn"));
    clear_btn_->setProperty("role", "ghost");
    copy_btn_ = new QPushButton(QStringLiteral("Copy"), content);
    copy_btn_->setObjectName(QStringLiteral("logCopyBtn"));
    copy_btn_->setProperty("role", "ghost");
    export_btn_ = new QPushButton(QStringLiteral("Export"), content);
    export_btn_->setObjectName(QStringLiteral("logExportBtn"));
    export_btn_->setProperty("role", "ghost");
    open_folder_btn_ = new QPushButton(QStringLiteral("Open Log Folder"), content);
    open_folder_btn_->setObjectName(QStringLiteral("logOpenFolderBtn"));
    open_folder_btn_->setProperty("role", "ghost");

    control_row->addWidget(refresh_btn_, 0);
    control_row->addWidget(clear_btn_, 0);
    control_row->addWidget(copy_btn_, 0);
    control_row->addWidget(export_btn_, 0);
    control_row->addWidget(open_folder_btn_, 0);
    layout->addLayout(control_row);

    status_label_ = new QLabel(content);
    status_label_->setProperty("labelRole", "logStatus");
    status_label_->setWordWrap(true);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_label_);

    log_viewer_ = new QPlainTextEdit(content);
    log_viewer_->setObjectName(QStringLiteral("logViewer"));
    log_viewer_->setReadOnly(true);
    log_viewer_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    log_viewer_->setMaximumBlockCount(AppLog::maxEntries());
    log_viewer_->setMinimumHeight(300);
    layout->addWidget(log_viewer_, 1);

    auto* footnote =
        new QLabel(QStringLiteral("Full session logs are written to %LOCALAPPDATA%\\ExoSnap\\logs."), content);
    footnote->setObjectName(QStringLiteral("logFootnote"));
    footnote->setProperty("labelRole", "subtle");
    footnote->setWordWrap(true);
    layout->addWidget(footnote);

    auto* centering_host = new QWidget();
    auto* ch = new QHBoxLayout(centering_host);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->addStretch(1);
    ch->addWidget(content, 10);
    ch->addStretch(1);
    outer->addWidget(centering_host, 1);

    search_debounce_timer_ = new QTimer(this);
    search_debounce_timer_->setSingleShot(true);
    search_debounce_timer_->setInterval(kSearchDebounceMs);

    connect(refresh_btn_, &QPushButton::clicked, this, &LogsPage::onRefresh);
    connect(clear_btn_, &QPushButton::clicked, this, &LogsPage::onClear);
    connect(copy_btn_, &QPushButton::clicked, this, &LogsPage::onCopy);
    connect(export_btn_, &QPushButton::clicked, this, &LogsPage::onExport);
    connect(open_folder_btn_, &QPushButton::clicked, this, &LogsPage::onOpenFolder);
    connect(severity_group_, &QButtonGroup::idClicked, this, &LogsPage::onFilterClicked);
    connect(search_edit_, &QLineEdit::textChanged, this, &LogsPage::onSearchTextChanged);
    connect(search_debounce_timer_, &QTimer::timeout, this, &LogsPage::applyPendingSearch);
    connect(auto_scroll_check_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled && log_viewer_)
            log_viewer_->verticalScrollBar()->setValue(log_viewer_->verticalScrollBar()->maximum());
    });
    connect(&AppLog::instance(), &AppLog::entriesAppended, this, &LogsPage::onEntriesAppended);
    connect(&AppLog::instance(), &AppLog::cleared, this, &LogsPage::onLogCleared);

    entries_ = AppLog::history();
    for (const LogEntry& entry : entries_)
        last_sequence_seen_ = std::max(last_sequence_seen_, entry.sequence);
    setSeverityFilter(SeverityFilter::All);
    rebuildVisibleEntries();
}

void LogsPage::setSeverityFilter(SeverityFilter filter) {
    filter_ = filter;
    if (severity_group_) {
        const QSignalBlocker blocker(severity_group_);
        for (auto* button : severity_group_->buttons()) {
            const bool selected = severity_group_->id(button) == static_cast<int>(filter);
            button->setChecked(selected);
            button->setProperty("qualitySegmentSelected", selected);
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
    }
    rebuildVisibleEntries();
}

LogsPage::SeverityFilter LogsPage::severityFilter() const noexcept {
    return filter_;
}

QString LogsPage::activeFilterName() const {
    return filterName(filter_);
}

void LogsPage::setSearchQuery(const QString& query) {
    search_query_ = query.trimmed();
    pending_search_query_ = search_query_;
    if (search_debounce_timer_)
        search_debounce_timer_->stop();
    if (search_edit_ && search_edit_->text() != query) {
        const QSignalBlocker blocker(search_edit_);
        search_edit_->setText(query);
    }
    rebuildVisibleEntries();
}

const QString& LogsPage::searchQuery() const noexcept {
    return search_query_;
}

void LogsPage::setAutoScrollEnabled(bool enabled) {
    if (!auto_scroll_check_)
        return;
    auto_scroll_check_->setChecked(enabled);
}

bool LogsPage::autoScrollEnabled() const {
    return auto_scroll_check_ == nullptr || auto_scroll_check_->isChecked();
}

int LogsPage::totalEntryCount() const {
    return entries_.size();
}

int LogsPage::visibleEntryCount() const {
    return visible_entries_.size();
}

QString LogsPage::oldestVisibleSeverityName() const {
    return visible_entries_.isEmpty() ? QString() : AppLog::severityLabel(visible_entries_.front().severity);
}

QString LogsPage::newestVisibleSeverityName() const {
    return visible_entries_.isEmpty() ? QString() : AppLog::severityLabel(visible_entries_.back().severity);
}

QString LogsPage::copyText() const {
    return entriesToText(visible_entries_);
}

bool LogsPage::exportToFile(const QString& path, QString* error) const {
    if (!synthetic_mode_)
        return AppLog::exportHistoryToFile(path, error);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Could not write log export: %1").arg(file.errorString());
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    for (const LogEntry& entry : entries_)
        stream << AppLog::formatEntry(entry) << '\n';
    stream.flush();
    return stream.status() == QTextStream::Ok;
}

void LogsPage::clearLogs() {
    if (synthetic_mode_) {
        entries_.clear();
        visible_entries_.clear();
        if (log_viewer_)
            log_viewer_->clear();
        updateActionState();
        updateStatusLabel(QStringLiteral("Log history cleared."));
        return;
    }
    AppLog::clear();
    updateStatusLabel(QStringLiteral("Log history cleared."));
}

int LogsPage::fullRebuildCountForTesting() const noexcept {
    return full_rebuild_count_;
}

int LogsPage::incrementalAppendCountForTesting() const noexcept {
    return incremental_append_count_;
}

void LogsPage::onRefresh() {
    if (!synthetic_mode_) {
        entries_ = AppLog::history();
        last_sequence_seen_ = 0;
        for (const LogEntry& entry : entries_)
            last_sequence_seen_ = std::max(last_sequence_seen_, entry.sequence);
    }
    rebuildVisibleEntries();
    updateStatusLabel(QStringLiteral("Refreshed."));
}

void LogsPage::onOpenFolder() {
    const QString path = AppLog::logFilePath();
    if (path.isEmpty()) {
        updateStatusLabel(QStringLiteral("No log folder is available yet."));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void LogsPage::onCopy() {
    const QString text = copyText();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    updateStatusLabel(QStringLiteral("Copied %1 visible entries.").arg(visible_entries_.size()));
}

void LogsPage::onExport() {
    const QString default_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString default_path =
        (default_dir.isEmpty() ? QDir::homePath() : default_dir) + QStringLiteral("/exosnap-log.txt");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Log"), default_path,
                                                      QStringLiteral("Text files (*.txt);;All files (*.*)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!exportToFile(path, &error)) {
        updateStatusLabel(QStringLiteral("Export failed: %1").arg(error));
        return;
    }
    updateStatusLabel(QStringLiteral("Exported complete history to %1.").arg(path));
}

void LogsPage::onClear() {
    clearLogs();
}

void LogsPage::onFilterClicked(int id) {
    setSeverityFilter(static_cast<SeverityFilter>(id));
}

void LogsPage::onSearchTextChanged(const QString& text) {
    pending_search_query_ = text.trimmed();
    if (search_debounce_timer_)
        search_debounce_timer_->start();
}

void LogsPage::applyPendingSearch() {
    if (search_query_ == pending_search_query_)
        return;
    search_query_ = pending_search_query_;
    rebuildVisibleEntries();
}

void LogsPage::onEntriesAppended(QVector<LogEntry> entries, int evicted_count) {
    if (synthetic_mode_ || entries.isEmpty())
        return;

    QVector<LogEntry> fresh_entries;
    fresh_entries.reserve(entries.size());
    for (const LogEntry& entry : entries) {
        if (entry.sequence != 0 && entry.sequence <= last_sequence_seen_)
            continue;
        fresh_entries.push_back(entry);
        last_sequence_seen_ = std::max(last_sequence_seen_, entry.sequence);
    }
    if (fresh_entries.isEmpty()) {
        updateStatusLabel();
        return;
    }

    if (evicted_count > 0) {
        entries_ = AppLog::history();
        last_sequence_seen_ = 0;
        for (const LogEntry& entry : entries_)
            last_sequence_seen_ = std::max(last_sequence_seen_, entry.sequence);
        rebuildVisibleEntries();
        updateStatusLabel(QStringLiteral("Oldest entries evicted from bounded history."));
        return;
    }

    entries_.reserve(entries_.size() + fresh_entries.size());
    for (const LogEntry& entry : fresh_entries)
        entries_.push_back(entry);
    appendMatchingEntries(fresh_entries);
    updateStatusLabel();
}

void LogsPage::onLogCleared() {
    entries_.clear();
    visible_entries_.clear();
    last_sequence_seen_ = 0;
    if (log_viewer_)
        log_viewer_->clear();
    updateActionState();
    updateStatusLabel();
}

void LogsPage::rebuildVisibleEntries() {
    visible_entries_.clear();
    visible_entries_.reserve(entries_.size());
    for (const LogEntry& entry : entries_) {
        if (matchesActiveFilters(entry))
            visible_entries_.push_back(entry);
    }
    replaceDocumentFromVisibleEntries();
    ++full_rebuild_count_;
    updateActionState();
    updateStatusLabel();
}

void LogsPage::appendMatchingEntries(const QVector<LogEntry>& entries) {
    QVector<LogEntry> matching;
    matching.reserve(entries.size());
    for (const LogEntry& entry : entries) {
        if (matchesActiveFilters(entry))
            matching.push_back(entry);
    }
    if (matching.isEmpty()) {
        updateActionState();
        return;
    }

    const int previous_scroll_value = log_viewer_->verticalScrollBar()->value();
    for (const LogEntry& entry : matching) {
        visible_entries_.push_back(entry);
        insertEntry(entry);
    }
    ++incremental_append_count_;
    if (autoScrollEnabled()) {
        log_viewer_->verticalScrollBar()->setValue(log_viewer_->verticalScrollBar()->maximum());
    } else {
        log_viewer_->verticalScrollBar()->setValue(previous_scroll_value);
    }
    updateActionState();
}

void LogsPage::insertEntry(const LogEntry& entry) {
    if (!log_viewer_)
        return;

    QTextCursor cursor(log_viewer_->document());
    cursor.movePosition(QTextCursor::End);
    if (!log_viewer_->document()->isEmpty())
        cursor.insertBlock();

    QTextBlockFormat block_format;
    block_format.setProperty(kSeverityBlockProperty, AppLog::severityKey(entry.severity));
    cursor.setBlockFormat(block_format);

    QTextCharFormat char_format = formatForSeverity(entry.severity);
    cursor.setCharFormat(char_format);
    cursor.insertText(AppLog::formatEntry(entry));
}

void LogsPage::replaceDocumentFromVisibleEntries() {
    if (!log_viewer_)
        return;

    const int previous_scroll_value = log_viewer_->verticalScrollBar()->value();
    log_viewer_->clear();
    for (const LogEntry& entry : visible_entries_)
        insertEntry(entry);

    if (autoScrollEnabled()) {
        log_viewer_->verticalScrollBar()->setValue(log_viewer_->verticalScrollBar()->maximum());
    } else {
        log_viewer_->verticalScrollBar()->setValue(previous_scroll_value);
    }
}

void LogsPage::updateActionState() {
    if (copy_btn_)
        copy_btn_->setEnabled(!visible_entries_.isEmpty());
    if (export_btn_)
        export_btn_->setEnabled(!entries_.isEmpty());
    if (clear_btn_)
        clear_btn_->setEnabled(!entries_.isEmpty());
    if (open_folder_btn_)
        open_folder_btn_->setEnabled(!AppLog::logFilePath().isEmpty());
}

void LogsPage::updateStatusLabel(const QString& feedback) {
    if (!status_label_)
        return;

    const QString search_part =
        search_query_.isEmpty() ? QStringLiteral("no search") : QStringLiteral("search \"%1\"").arg(search_query_);
    QString text = QStringLiteral("Showing %1 of %2 entries | %3 | %4")
                       .arg(visible_entries_.size())
                       .arg(entries_.size())
                       .arg(activeFilterName(), search_part);
    const QString path = AppLog::logFilePath();
    if (!path.isEmpty())
        text += QStringLiteral(" | %1").arg(path);
    if (!feedback.isEmpty())
        text += QStringLiteral(" | %1").arg(feedback);
    status_label_->setText(text);
}

bool LogsPage::matchesActiveFilters(const LogEntry& entry) const {
    switch (filter_) {
    case SeverityFilter::All:
        break;
    case SeverityFilter::Info:
        if (entry.severity != LogSeverity::Info)
            return false;
        break;
    case SeverityFilter::Issues:
        if (!isIssueSeverity(entry.severity))
            return false;
        break;
    }

    if (search_query_.isEmpty())
        return true;

    return entry.message.contains(search_query_, Qt::CaseInsensitive) ||
           entry.category.contains(search_query_, Qt::CaseInsensitive);
}

QTextCharFormat LogsPage::formatForSeverity(LogSeverity severity) const {
    QTextCharFormat format;
    format.setFontFamily(QStringLiteral("JetBrains Mono"));
    format.setProperty(kSeverityTextProperty, AppLog::severityKey(severity));

    QColor color(QStringLiteral("#D7DDE8"));
    switch (severity) {
    case LogSeverity::Debug:
        color = QColor(QStringLiteral("#7D8594"));
        break;
    case LogSeverity::Info:
        color = QColor(QStringLiteral("#D7DDE8"));
        break;
    case LogSeverity::Warning:
        color = QColor(QStringLiteral("#E6C57C"));
        break;
    case LogSeverity::Error:
        color = QColor(QStringLiteral("#E0786C"));
        break;
    }
    format.setForeground(color);
    return format;
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void LogsPage::applyVisualScenario(const visual::VisualScenario& scenario) {
    QVector<LogEntry> entries;
    if (scenario.id == QStringLiteral("logs-empty")) {
        entries = {};
    } else if (scenario.id == QStringLiteral("logs-search-results")) {
        entries = visualSearchEntries();
    } else if (scenario.id == QStringLiteral("logs-long-message")) {
        entries = visualLongMessageEntries();
    } else if (scenario.id == QStringLiteral("logs-buffer-truncated")) {
        entries = visualTruncatedEntries();
    } else {
        entries = visualAllLevelsEntries();
    }

    setSyntheticEntriesForVisualTest(std::move(entries));
    setAutoScrollEnabled(scenario.log_auto_scroll);
    setSeverityFilter(filterFromVisual(scenario.log_filter));
    setSearchQuery(scenario.log_search_query);
}

void LogsPage::setSyntheticEntriesForVisualTest(QVector<LogEntry> entries) {
    synthetic_mode_ = true;
    entries_ = std::move(entries);
    last_sequence_seen_ = 0;
    visible_entries_.clear();
    rebuildVisibleEntries();
}
#endif

} // namespace exosnap
