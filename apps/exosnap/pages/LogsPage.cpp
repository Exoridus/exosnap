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

#include "../ui/theme/ExoSnapMetrics.h"

namespace exosnap {

namespace {

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "subtitle");
    l->setWordWrap(true);
    return l;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "section");
    return l;
}

QLabel* makePlaceholderRow(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("panelRole", "placeholder");
    l->setProperty("labelRole", "mono");
    return l;
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
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

    // Log viewer surface
    layout->addWidget(makeSectionLabel("Log Viewer Surface", content));
    auto* viewer_panel = makePanel(content);
    auto* viewer_layout = new QVBoxLayout(viewer_panel);
    viewer_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                      ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    viewer_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    viewer_layout->addWidget(
        makeSubLabel("Runtime and startup logs are displayed here once the log store is wired.", viewer_panel));
    viewer_layout->addWidget(makePlaceholderRow("[viewer] log stream not initialized", viewer_panel));
    viewer_layout->addWidget(makePlaceholderRow("[viewer] waiting for integrated session source", viewer_panel));
    layout->addWidget(viewer_panel);

    // Filters / scope placeholder
    layout->addWidget(makeSectionLabel("Filters / Scope Placeholder", content));
    auto* filters_panel = makePanel(content);
    auto* filters_layout = new QVBoxLayout(filters_panel);
    filters_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                       ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    auto* filters_note = new QLabel("Filter and scope controls are introduced in a later pass.", filters_panel);
    filters_note->setProperty("labelRole", "subtle");
    filters_layout->addWidget(filters_note);
    layout->addWidget(filters_panel);

    // Session export actions
    layout->addWidget(makeSectionLabel("Export / Session Trace", content));
    auto* actions_panel = makePanel(content);
    auto* actions_layout = new QVBoxLayout(actions_panel);
    actions_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                       ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    actions_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* export_btn = new QPushButton("Export Selected", actions_panel);
    export_btn->setProperty("role", "ghost");
    export_btn->setEnabled(false);

    auto* open_btn = new QPushButton("Open Log Folder", actions_panel);
    open_btn->setProperty("role", "ghost");

    btn_row->addWidget(export_btn);
    btn_row->addWidget(open_btn);
    btn_row->addStretch();
    actions_layout->addLayout(btn_row);
    auto* actions_note = new QLabel("Export remains disabled until session log selection is available.", actions_panel);
    actions_note->setProperty("labelRole", "subtle");
    actions_layout->addWidget(actions_note);
    layout->addWidget(actions_panel);

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
