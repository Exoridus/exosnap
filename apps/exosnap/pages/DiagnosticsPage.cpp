#include "DiagnosticsPage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
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

QFrame* makeCategoryRow(const QString& name, const QString& hint, QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setStyleSheet("QFrame { background: #141A26; border-radius: 6px; }");
    auto* row_layout = new QHBoxLayout(f);
    row_layout->setContentsMargins(14, 10, 14, 10);
    auto* label = new QLabel(name, f);
    label->setStyleSheet("color: #C0C4CC; font-size: 13px; font-weight: 600;");
    auto* hint_label = new QLabel(hint, f);
    hint_label->setStyleSheet("color: #4A5166; font-size: 12px;");
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
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    layout->addWidget(makeTitle("Diagnostics", content));
    layout->addWidget(makeSubLabel("System capability probes and blocker status.", content));

    // Overall status
    status_label_ = new QLabel("Overall status: Not checked", content);
    status_label_->setStyleSheet("color: #C0C4CC; font-size: 13px;");
    layout->addWidget(status_label_);

    last_check_label_ = new QLabel("Last check: —", content);
    last_check_label_->setStyleSheet("color: #6A7280; font-size: 12px;");
    layout->addWidget(last_check_label_);

    // Action buttons
    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(8);

    auto* run_btn = new QPushButton("Run System & Pipeline Check", content);
    run_btn->setStyleSheet("QPushButton { background: #2468C0; border: none; border-radius: 4px;"
                           " padding: 8px 18px; color: #FFFFFF; font-weight: 600; }"
                           "QPushButton:hover { background: #3078D0; }"
                           "QPushButton:pressed { background: #1A58B0; }");

    auto* export_btn = new QPushButton("Export Diagnostic Report", content);
    export_btn->setStyleSheet("QPushButton { background: #252C3C; border: 1px solid #3A4254;"
                              " border-radius: 4px; padding: 8px 18px; color: #C0C4CC; }"
                              "QPushButton:hover { background: #2E3648; }"
                              "QPushButton:disabled { background: #1A2030; color: #454C5E; }");
    export_btn->setEnabled(false);

    btn_row->addWidget(run_btn);
    btn_row->addWidget(export_btn);
    btn_row->addStretch();
    layout->addLayout(btn_row);

    // Summary
    summary_label_ = new QLabel("Run a check to see results.", content);
    summary_label_->setStyleSheet("color: #6A7280; font-size: 12px;");
    layout->addWidget(summary_label_);

    // Category groups
    layout->addWidget(makeSectionLabel("Categories", content));
    layout->addWidget(makeSubLabel("Results appear here after running a check.", content));

    const char* kCategories[] = {
        "Operating System", "GPU & Encoder", "Display", "Audio", "Storage", "Pipeline", "Settings Compatibility",
    };
    for (const char* cat : kCategories)
        layout->addWidget(makeCategoryRow(cat, "—", content));

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
