#include "LogsPage.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include "../diagnostics/AppLog.h"
#include "../ui/theme/ExoSnapMetrics.h"

namespace exosnap {
namespace {

constexpr int kMaxLogLines = 500;

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "section");
    return l;
}

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
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    auto* header_row = new QHBoxLayout();
    header_row->addWidget(makeSectionLabel("Application Log", content));
    header_row->addStretch();

    refresh_btn_ = new QPushButton("Refresh", content);
    refresh_btn_->setProperty("role", "ghost");

    open_folder_btn_ = new QPushButton("Open Log Folder", content);
    open_folder_btn_->setProperty("role", "ghost");

    header_row->addWidget(refresh_btn_);
    header_row->addWidget(open_folder_btn_);
    layout->addLayout(header_row);

    status_label_ = new QLabel(content);
    status_label_->setProperty("labelRole", "muted");
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);

    log_viewer_ = new QPlainTextEdit(content);
    log_viewer_->setReadOnly(true);
    log_viewer_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_viewer_->setFont(QFont(QStringLiteral("JetBrains Mono, Cascadia Mono, Consolas"), 10));
    log_viewer_->setStyleSheet(
        QStringLiteral("QPlainTextEdit { background-color: #0d0d0d; color: #c0c0c0; border: 1px solid #2a2a2a; "
                       "border-radius: 4px; padding: 8px; }"));
    log_viewer_->setMinimumHeight(300);
    layout->addWidget(log_viewer_, 1);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(refresh_btn_, &QPushButton::clicked, this, &LogsPage::onRefresh);
    connect(open_folder_btn_, &QPushButton::clicked, this, &LogsPage::onOpenFolder);

    reloadLogContent();
}

void LogsPage::reloadLogContent() {
    const QString path = diagnostics::LogFilePath();
    if (path.isEmpty()) {
        status_label_->setText("No log file found yet.");
        log_viewer_->setPlainText({});
        return;
    }

    QFileInfo info(path);
    if (!info.exists()) {
        status_label_->setText("Log file not yet created. Log entries appear once recording or probing starts.");
        log_viewer_->setPlainText({});
        return;
    }

    const QString content = readLogTail(path, kMaxLogLines);
    log_viewer_->setPlainText(content);
    status_label_->setText(QStringLiteral("Showing last %1 lines from %2").arg(kMaxLogLines).arg(path));
}

void LogsPage::onRefresh() {
    reloadLogContent();
}

void LogsPage::onOpenFolder() {
    const QString path = diagnostics::LogFilePath();
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

} // namespace exosnap
