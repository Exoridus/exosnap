#include "LogsPage.h"

#include <QBoxLayout>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
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
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceMd);

    // Toolbar: log-file path / status on the left, raw-viewer actions on the right.
    auto* toolbar = new ui::widgets::SectionRuleHeader(QStringLiteral("APPLICATION LOG"), this);
    layout->addWidget(toolbar);

    action_row_layout_ = new QBoxLayout(QBoxLayout::LeftToRight);
    action_row_layout_->setSpacing(M::kSpaceSm);

    status_label_ = new QLabel(this);
    status_label_->setProperty("labelRole", "logStatus");
    status_label_->setWordWrap(true);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    action_row_layout_->addWidget(status_label_, 1);

    refresh_btn_ = new QPushButton(QStringLiteral("Refresh"), this);
    refresh_btn_->setProperty("role", "fieldAction");
    open_folder_btn_ = new QPushButton(QStringLiteral("Open Log Folder"), this);
    open_folder_btn_->setProperty("role", "fieldAction");
    action_row_layout_->addWidget(refresh_btn_, 0);
    action_row_layout_->addWidget(open_folder_btn_, 0);
    layout->addLayout(action_row_layout_);

    log_viewer_ = new QPlainTextEdit(this);
    log_viewer_->setObjectName(QStringLiteral("logViewer"));
    log_viewer_->setReadOnly(true);
    log_viewer_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono;
    mono.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                      QStringLiteral("Lucida Console")});
    mono.setStyleHint(QFont::TypeWriter);
    mono.setFixedPitch(true);
    mono.setPointSize(10);
    log_viewer_->setFont(mono);
    log_viewer_->setTabStopDistance(log_viewer_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    log_viewer_->setMinimumHeight(300);
    layout->addWidget(log_viewer_, 1);

    connect(refresh_btn_, &QPushButton::clicked, this, &LogsPage::onRefresh);
    connect(open_folder_btn_, &QPushButton::clicked, this, &LogsPage::onOpenFolder);

    reloadLogContent();
}

void LogsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!action_row_layout_) {
        return;
    }
    const QBoxLayout::Direction desired = width() < 980 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (action_row_layout_->direction() != desired) {
        action_row_layout_->setDirection(desired);
    }
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
