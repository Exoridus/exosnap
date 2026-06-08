#include "LogsPage.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include "../diagnostics/AppLog.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/SectionRuleHeader.h"

namespace exosnap {
namespace {

using M = ui::theme::ExoSnapMetrics;

constexpr int kMaxLogLines = 500;

QString readLogTail(const QString& path, int max_lines) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList lines;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        lines.append(stream.readLine());
        if (lines.size() > max_lines)
            lines.removeFirst();
    }
    return lines.join(QStringLiteral("\n"));
}

} // namespace

LogsPage::LogsPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* content = new QWidget();
    content->setMaximumWidth(1320);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceMd);

    // Toolbar: log-file path / status on the left, raw-viewer actions on the right.
    auto* toolbar = new ui::widgets::SectionRuleHeader(QStringLiteral("APPLICATION LOG"), content);
    layout->addWidget(toolbar);

    auto* action_row = new QHBoxLayout();
    action_row->setSpacing(M::kSpaceSm);

    status_label_ = new QLabel(content);
    status_label_->setProperty("labelRole", "logStatus");
    status_label_->setWordWrap(true);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    action_row->addWidget(status_label_, 1);

    refresh_btn_ = new QPushButton(QStringLiteral("Refresh"), content);
    refresh_btn_->setObjectName(QStringLiteral("logRefreshBtn"));
    refresh_btn_->setProperty("role", "ghost");
    copy_btn_ = new QPushButton(QStringLiteral("Copy"), content);
    copy_btn_->setObjectName(QStringLiteral("logCopyBtn"));
    copy_btn_->setProperty("role", "ghost");
    open_folder_btn_ = new QPushButton(QStringLiteral("Open Log Folder"), content);
    open_folder_btn_->setObjectName(QStringLiteral("logOpenFolderBtn"));
    open_folder_btn_->setProperty("role", "ghost");
    action_row->addWidget(refresh_btn_, 0);
    action_row->addWidget(copy_btn_, 0);
    action_row->addWidget(open_folder_btn_, 0);
    layout->addLayout(action_row);

    log_viewer_ = new QPlainTextEdit(content);
    log_viewer_->setObjectName(QStringLiteral("logViewer"));
    log_viewer_->setReadOnly(true);
    log_viewer_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_viewer_->setMinimumHeight(300);
    layout->addWidget(log_viewer_, 1);

    auto* footnote = new QLabel(
        QStringLiteral("Full logs are written to %LOCALAPPDATA%\\ExoSnap\\logs — newest entries at the bottom."),
        content);
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

    connect(refresh_btn_, &QPushButton::clicked, this, &LogsPage::onRefresh);
    connect(copy_btn_, &QPushButton::clicked, this, &LogsPage::onCopy);
    connect(open_folder_btn_, &QPushButton::clicked, this, &LogsPage::onOpenFolder);

    reloadLogContent();
}

void LogsPage::reloadLogContent() {
    const QString path = diagnostics::LogFilePath();
    if (path.isEmpty()) {
        status_label_->setText(QStringLiteral("No log file found yet."));
        log_viewer_->setPlainText({});
        copy_btn_->setEnabled(false);
        return;
    }

    QFileInfo info(path);
    if (!info.exists()) {
        status_label_->setText(
            QStringLiteral("Log file not yet created. Log entries appear once recording or probing starts."));
        log_viewer_->setPlainText({});
        copy_btn_->setEnabled(false);
        return;
    }

    const QString content = readLogTail(path, kMaxLogLines);
    log_viewer_->setPlainText(content);
    copy_btn_->setEnabled(!content.isEmpty());
    status_label_->setText(QStringLiteral("Showing last %1 lines  \xc2\xb7  %2").arg(kMaxLogLines).arg(path));
}

void LogsPage::onRefresh() {
    reloadLogContent();
}

void LogsPage::onCopy() {
    if (!log_viewer_)
        return;
    const QString text = log_viewer_->toPlainText();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
}

void LogsPage::onOpenFolder() {
    const QString path = diagnostics::LogFilePath();
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

} // namespace exosnap
