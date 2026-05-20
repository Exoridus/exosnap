#include "DiagnosticsPage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
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

QFrame* makeCategoryRow(const QString& name, const QString& hint, QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setProperty("panelRole", "compactRow");
    auto* row_layout = new QHBoxLayout(f);
    row_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm,
                                   ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm);
    auto* label = new QLabel(name, f);
    label->setProperty("labelRole", "body");
    auto* hint_label = new QLabel(hint, f);
    hint_label->setProperty("labelRole", "subtle");
    row_layout->addWidget(label);
    row_layout->addStretch();
    row_layout->addWidget(hint_label);
    return f;
}

} // namespace

DiagnosticsPage::DiagnosticsPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // Readiness overview
    layout->addWidget(makeSectionLabel("Readiness / Capability Overview", content));
    auto* overview_panel = new QFrame(content);
    overview_panel->setProperty("panelRole", "panel");
    auto* overview_layout = new QVBoxLayout(overview_panel);
    overview_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    overview_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    overview_layout->addWidget(
        makeSubLabel("Diagnostics are designed to block invalid recording states early.", overview_panel));

    status_label_ = new QLabel("Overall status: Not checked", overview_panel);
    status_label_->setProperty("labelRole", "body");
    overview_layout->addWidget(status_label_);

    last_check_label_ = new QLabel("Last check: —", overview_panel);
    last_check_label_->setProperty("labelRole", "subtle");
    overview_layout->addWidget(last_check_label_);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* run_btn = new QPushButton("Run System & Pipeline Check", overview_panel);
    run_btn->setProperty("role", "primary");

    auto* export_btn = new QPushButton("Export Diagnostic Report", overview_panel);
    export_btn->setProperty("role", "ghost");
    export_btn->setEnabled(false);

    btn_row->addWidget(run_btn);
    btn_row->addWidget(export_btn);
    btn_row->addStretch();
    overview_layout->addLayout(btn_row);

    summary_label_ = new QLabel("Run a check to see results.", overview_panel);
    summary_label_->setProperty("labelRole", "muted");
    overview_layout->addWidget(summary_label_);
    layout->addWidget(overview_panel);

    // Blockers / warnings placeholder
    layout->addWidget(makeSectionLabel("Blockers / Warnings", content));
    auto* blockers_panel = new QFrame(content);
    blockers_panel->setProperty("panelRole", "panel");
    auto* blockers_layout = new QVBoxLayout(blockers_panel);
    blockers_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    auto* blockers_note =
        new QLabel("Detailed blocker and warning lists are shown here after a check.", blockers_panel);
    blockers_note->setProperty("labelRole", "subtle");
    blockers_layout->addWidget(blockers_note);
    layout->addWidget(blockers_panel);

    // Categories
    layout->addWidget(makeSectionLabel("System Categories", content));
    auto* categories_panel = new QFrame(content);
    categories_panel->setProperty("panelRole", "panel");
    auto* categories_layout = new QVBoxLayout(categories_panel);
    categories_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                          ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    categories_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);
    categories_layout->addWidget(
        makeSubLabel("Each category is evaluated as part of readiness gating.", categories_panel));

    const char* kCategories[] = {
        "Operating System", "GPU & Encoder", "Display", "Audio", "Storage", "Pipeline", "Settings Compatibility",
    };
    for (const char* cat : kCategories)
        categories_layout->addWidget(makeCategoryRow(cat, "Pending", categories_panel));
    layout->addWidget(categories_panel);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->addWidget(scroll);

    connect(run_btn, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
}

void DiagnosticsPage::onRunCheck() {
    status_label_->setText("Overall status: Checking…");
    last_check_label_->setText("Last check: running…");
    summary_label_->setText("Check in progress.");
}

} // namespace exosnap
