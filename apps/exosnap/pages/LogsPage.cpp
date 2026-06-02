#include "LogsPage.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
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
    content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceMd);

    // Toolbar: log-file path / status on the left, raw-viewer actions on the right.
    auto* toolbar = new ui::widgets::SectionRuleHeader(QStringLiteral("APPLICATION LOG"), this);
    layout->addWidget(toolbar);

    auto* action_row = new QHBoxLayout();
    action_row->setSpacing(M::kSpaceSm);

    status_label_ = new QLabel(this);
    status_label_->setProperty("labelRole", "logStatus");
    status_label_->setWordWrap(true);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    action_row->addWidget(status_label_, 1);

    refresh_btn_ = new QPushButton(QStringLiteral("Refresh"), this);
    refresh_btn_->setProperty("role", "ghost");
    open_folder_btn_ = new QPushButton(QStringLiteral("Open Log Folder"), this);
    open_folder_btn_->setProperty("role", "ghost");
    action_row->addWidget(refresh_btn_, 0);
    action_row->addWidget(open_folder_btn_, 0);
    layout->addLayout(action_row);

    log_viewer_ = new QPlainTextEdit(this);
    log_viewer_->setObjectName(QStringLiteral("logViewer"));
    log_viewer_->setReadOnly(true);
    log_viewer_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_viewer_->setMinimumHeight(300);
    layout->addWidget(log_viewer_, 1);

    auto* centering_host = new QWidget();
    auto* ch = new QHBoxLayout(centering_host);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->addStretch(1);
    ch->addWidget(content, 0);
    ch->addStretch(1);
    outer->addWidget(centering_host, 1);

    connect(refresh_btn_, &QPushButton::clicked, this, &LogsPage::onRefresh);
    connect(open_folder_btn_, &QPushButton::clicked, this, &LogsPage::onOpenFolder);

    reloadLogContent();
}

void LogsPage::reloadLogContent() {
    const QString path = diagnostics::LogFilePath();
    if (path.isEmpty()) {
        status_label_->setText(QStringLiteral("No log file found yet."));
        log_viewer_->setPlainText({});
        return;
    }

    QFileInfo info(path);
    if (!info.exists()) {
        status_label_->setText(
            QStringLiteral("Log file not yet created. Log entries appear once recording or probing starts."));
        log_viewer_->setPlainText({});
        return;
    }

    const QString content = readLogTail(path, kMaxLogLines);
    log_viewer_->setPlainText(content);
    status_label_->setText(QStringLiteral("Showing last %1 lines  \xc2\xb7  %2").arg(kMaxLogLines).arg(path));
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
