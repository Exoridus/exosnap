#include "LogsPage.h"

#include <QButtonGroup>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
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
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"
#include "../ui/theme/LucideIcon.h"
#include "../ui/widgets/ExoCheckBox.h"
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

    // D3 toolbar: LEFT = segmented All/Info/Issues + search (flex) + Auto-scroll, gap 8
    //             RIGHT = Copy + Export… both ghost sm, gap 8; 16px between clusters
    auto* control_row = new QHBoxLayout();
    control_row->setSpacing(0); // spacing managed per cluster

    // Left cluster
    auto* left_cluster = new QWidget(content);
    auto* left_layout = new QHBoxLayout(left_cluster);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(M::kSpaceSm);

    auto* segmented = new QWidget(left_cluster);
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
    left_layout->addWidget(segmented, 0);

    search_edit_ = new QLineEdit(left_cluster);
    search_edit_->setObjectName(QStringLiteral("logSearchEdit"));
    search_edit_->setPlaceholderText(QStringLiteral("Search category or message"));
    search_edit_->setClearButtonEnabled(true);
    // Leading magnifier glyph so the field reads as a search affordance (v10).
    // No bundled search.svg asset exists; the shared Lucide "search" path is rendered
    // through QSvgRenderer and tinted to the muted-ink token.
    search_edit_->addAction(ui::theme::lucideIcon(QStringLiteral("search"),
                                                  QString::fromUtf8(ui::theme::ExoSnapPalette::kText2), 14,
                                                  search_edit_->devicePixelRatioF()),
                            QLineEdit::LeadingPosition);
    left_layout->addWidget(search_edit_, 1);

    auto_scroll_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Auto-scroll"), left_cluster);
    auto_scroll_check_->setObjectName(QStringLiteral("logAutoScrollToggle"));
    auto_scroll_check_->setChecked(true);
    left_layout->addWidget(auto_scroll_check_, 0);

    // Right cluster
    auto* right_cluster = new QWidget(content);
    auto* right_layout = new QHBoxLayout(right_cluster);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(M::kSpaceSm);

    copy_btn_ = new QPushButton(QStringLiteral("Copy"), right_cluster);
    copy_btn_->setObjectName(QStringLiteral("logCopyBtn"));
    copy_btn_->setProperty("role", "ghost");
    copy_btn_->setProperty("logToolbarGhost", true);
    export_btn_ = new QPushButton(QStringLiteral("Export\xe2\x80\xa6"), right_cluster); // "Export…"
    export_btn_->setObjectName(QStringLiteral("logExportBtn"));
    export_btn_->setProperty("role", "ghost");
    export_btn_->setProperty("logToolbarGhost", true);
    right_layout->addWidget(copy_btn_);
    right_layout->addWidget(export_btn_);

    control_row->addWidget(left_cluster, 1);
    control_row->addSpacing(16);
    control_row->addWidget(right_cluster, 0);
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

    // D3 footer: path portion is a clickable link that opens the folder.
    // Using a QLabel with rich text for the link portion; folder_link_ holds just the
    // clickable path label so we can update it when the log path becomes known.
    auto* footnote_row = new QHBoxLayout();
    footnote_row->setContentsMargins(0, 0, 0, 0);
    footnote_row->setSpacing(4);

    auto* footnote_prefix = new QLabel(QStringLiteral("Full session logs are written to"), content);
    footnote_prefix->setObjectName(QStringLiteral("logFootnote"));
    footnote_prefix->setProperty("labelRole", "subtle");
    footnote_row->addWidget(footnote_prefix, 0);

    folder_link_ = new QLabel(content);
    folder_link_->setObjectName(QStringLiteral("logFolderLink"));
    folder_link_->setProperty("labelRole", "logFolderLink");
    folder_link_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    folder_link_->setOpenExternalLinks(false);
    folder_link_->setCursor(Qt::PointingHandCursor);
    folder_link_->setText(QStringLiteral("%LOCALAPPDATA%\\ExoSnap\\logs."));
    footnote_row->addWidget(folder_link_, 0);
    footnote_row->addStretch(1);

    connect(folder_link_, &QLabel::linkActivated, this, [this](const QString&) { onOpenFolder(); });
    layout->addLayout(footnote_row);

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

    // D3: refresh_btn_, clear_btn_, open_folder_btn_ removed from toolbar.
    connect(copy_btn_, &QPushButton::clicked, this, &LogsPage::onCopy);
    connect(export_btn_, &QPushButton::clicked, this, &LogsPage::onExport);
    connect(severity_group_, &QButtonGroup::idClicked, this, &LogsPage::onFilterClicked);
    connect(search_edit_, &QLineEdit::textChanged, this, &LogsPage::onSearchTextChanged);
    connect(search_debounce_timer_, &QTimer::timeout, this, &LogsPage::applyPendingSearch);
    connect(auto_scroll_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool enabled) {
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

    // Render each column with its own colour so level and category read as distinct
    // visual lanes, matching the v10 LogPalette (log-time / level / log-cat / message).
    const QTextCharFormat sev_format = formatForSeverity(entry.severity);
    const QTextCharFormat cat_format = formatForCategory();

    // Timestamp — quietest column (log-time / dim).  Fixed width: 23 chars.
    {
        const ui::theme::ExoTheme& theme = ui::theme::ActiveTheme();
        QColor ts_color = ui::theme::ParseThemeColor(theme.log.time);
        if (!ts_color.isValid())
            ts_color = QColor(QStringLiteral("#65656A"));
        QTextCharFormat ts_format;
        ts_format.setFontFamilies(sev_format.fontFamilies().toStringList());
        ts_format.setProperty(kSeverityTextProperty, AppLog::severityKey(entry.severity));
        ts_format.setForeground(ts_color);
        cursor.setCharFormat(ts_format);
        cursor.insertText(entry.timestamp.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")));
    }

    // Level — coloured by severity, rendered a touch smaller than the message so it
    // reads as a quieter secondary lane (v10: level ~10px vs the ~11.5px message base).
    // Padded to [WARNING] width (7 chars) so the category column always starts on the
    // same character position (~64px lane).
    {
        // WARNING is the widest label (7 chars); pad all labels to 7 inside the brackets.
        const QString label = AppLog::severityLabel(entry.severity).leftJustified(7, QLatin1Char(' '));
        QTextCharFormat level_format = sev_format;
        QFont level_font(QStringLiteral("IBM Plex Mono"));
        level_font.setPixelSize(10);
        level_format.setFont(level_font, QTextCharFormat::FontPropertiesSpecifiedOnly);
        cursor.setCharFormat(level_format);
        cursor.insertText(QStringLiteral(" [") + label + QStringLiteral("]"));
    }

    // Category — dedicated log-cat colour, visually decoupled from level and accent.
    {
        const QString category = entry.category.trimmed();
        if (!category.isEmpty()) {
            cursor.setCharFormat(cat_format);
            cursor.insertText(QStringLiteral(" [") + category + QStringLiteral("]"));
        }
    }

    // Message — primary ink (theme.ink / text0).
    {
        const ui::theme::ExoTheme& theme = ui::theme::ActiveTheme();
        QColor ink_color = QColor(QString::fromUtf8(theme.ink));
        if (!ink_color.isValid())
            ink_color = QColor(QStringLiteral("#F1F1EF"));
        QTextCharFormat msg_fmt;
        msg_fmt.setFontFamilies(sev_format.fontFamilies().toStringList());
        msg_fmt.setProperty(kSeverityTextProperty, AppLog::severityKey(entry.severity));
        msg_fmt.setForeground(ink_color);
        cursor.setCharFormat(msg_fmt);
        cursor.insertText(QStringLiteral(" ") + entry.message.trimmed());
    }
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
    // D3: folder link text updated when log path is known.
    if (folder_link_) {
        const QString path = AppLog::logFilePath();
        if (!path.isEmpty()) {
            // Show just filename + parent dir, middle-elided to ~40 chars (#11).
            const QFileInfo fi(path);
            const QString display = fi.dir().dirName() + QStringLiteral("/") + fi.fileName();
            const QString elided = folder_link_->fontMetrics().elidedText(display, Qt::ElideMiddle, 320);
            // Trailing full stop sits outside the <a> anchor so it ends the sentence
            // without becoming part of the clickable link.
            folder_link_->setText(QStringLiteral("<a href='folder'>%1</a>.").arg(elided));
            folder_link_->setToolTip(path);
        }
    }
}

void LogsPage::updateStatusLabel(const QString& feedback) {
    if (!status_label_)
        return;

    const QString search_part =
        search_query_.isEmpty() ? QStringLiteral("no search") : QStringLiteral("search \"%1\"").arg(search_query_);
    // #11: Show only filename + parent dir in status; full path in footer link tooltip.
    QString text = QStringLiteral("Showing %1 of %2 entries \xc2\xb7 %3 \xc2\xb7 %4") // " · " middle dot
                       .arg(visible_entries_.size())
                       .arg(entries_.size())
                       .arg(activeFilterName(), search_part);
    if (!feedback.isEmpty())
        text += QStringLiteral(" \xc2\xb7 %1").arg(feedback); // " · " middle dot
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
    format.setFontFamilies({QStringLiteral("IBM Plex Mono")});
    format.setProperty(kSeverityTextProperty, AppLog::severityKey(severity));

    // Level colours from the active log palette (theme-tuned, same semantic hues as v10).
    const ui::theme::ExoTheme& theme = ui::theme::ActiveTheme();
    QColor color = ui::theme::ParseThemeColor(theme.log.info);
    switch (severity) {
    case LogSeverity::Debug:
        color = ui::theme::ParseThemeColor(theme.log.debug);
        break;
    case LogSeverity::Info:
        color = ui::theme::ParseThemeColor(theme.log.info);
        break;
    case LogSeverity::Warning:
        color = ui::theme::ParseThemeColor(theme.log.warn);
        break;
    case LogSeverity::Error:
        color = ui::theme::ParseThemeColor(theme.log.error);
        break;
    }
    if (!color.isValid())
        color = QColor(QStringLiteral("#C5C5C3"));
    format.setForeground(color);
    return format;
}

QTextCharFormat LogsPage::formatForCategory() const {
    QTextCharFormat format;
    format.setFontFamilies({QStringLiteral("IBM Plex Mono")});
    // Category column uses the dedicated log-cat palette token — visually decoupled
    // from the primary accent (v10 LogPalette rule: "never var(--ac)").
    const ui::theme::ExoTheme& theme = ui::theme::ActiveTheme();
    QColor color = ui::theme::ParseThemeColor(theme.log.cat);
    if (!color.isValid())
        color = QColor(QStringLiteral("#7FB7D9"));
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
