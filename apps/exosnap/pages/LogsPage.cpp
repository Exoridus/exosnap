#include "LogsPage.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace exosnap {

namespace {

QLabel* makeTitle(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 22px; font-weight: 600; color: #E8EAED;");
    return l;
}

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8A9099; font-size: 13px;");
    l->setWordWrap(true);
    return l;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 13px; font-weight: 600; color: #C0C4CC; margin-top: 4px;");
    return l;
}

QLabel* makePlaceholderRow(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #4A5166; font-size: 12px; padding: 10px 14px;"
                     " background: #141A26; border-radius: 6px;");
    return l;
}

} // namespace

LogsPage::LogsPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    layout->addWidget(makeTitle("Logs", content));
    layout->addWidget(makeSubLabel("Startup and runtime log output.", content));

    // Sessions section
    layout->addWidget(makeSectionLabel("Sessions", content));
    layout->addWidget(makePlaceholderRow("No sessions recorded yet.", content));

    // Events section
    layout->addWidget(makeSectionLabel("Events", content));
    layout->addWidget(makePlaceholderRow("No events recorded yet.", content));

    // Action buttons
    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(8);

    auto* export_btn = new QPushButton("Export Selected", content);
    export_btn->setStyleSheet("QPushButton { background: #252C3C; border: 1px solid #3A4254;"
                              " border-radius: 4px; padding: 7px 16px; color: #C0C4CC; }"
                              "QPushButton:hover { background: #2E3648; }"
                              "QPushButton:disabled { background: #1A2030; color: #454C5E; }");
    export_btn->setEnabled(false);

    auto* open_btn = new QPushButton("Open Log Folder", content);
    open_btn->setStyleSheet("QPushButton { background: #252C3C; border: 1px solid #3A4254;"
                            " border-radius: 4px; padding: 7px 16px; color: #C0C4CC; }"
                            "QPushButton:hover { background: #2E3648; }");

    btn_row->addWidget(export_btn);
    btn_row->addWidget(open_btn);
    btn_row->addStretch();
    layout->addLayout(btn_row);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(export_btn, &QPushButton::clicked, this, &LogsPage::onExport);
    connect(open_btn, &QPushButton::clicked, this, &LogsPage::onOpenFolder);
}

void LogsPage::onExport() {
    // Export functionality will be wired when the log store exists.
}

void LogsPage::onOpenFolder() {
    QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    QDesktopServices::openUrl(QUrl::fromLocalFile(videos + "/Exosnap"));
}

} // namespace exosnap
