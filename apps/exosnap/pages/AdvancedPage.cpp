#include "AdvancedPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
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

QCheckBox* makeCheck(const QString& text, QWidget* parent) {
    auto* c = new QCheckBox(text, parent);
    c->setStyleSheet("QCheckBox { color: #C8CBD0; font-size: 13px; spacing: 6px; }"
                     "QCheckBox::indicator { width: 14px; height: 14px; border: 2px solid #3A4254;"
                     " border-radius: 2px; background: #1A2133; }"
                     "QCheckBox::indicator:checked { border: 2px solid #2468C0; background: #2468C0; }");
    return c;
}

} // namespace

AdvancedPage::AdvancedPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    layout->addWidget(makeTitle("Advanced", content));
    layout->addWidget(makeSubLabel("Advanced encoding and capture options.", content));

    // Warning note
    auto* note = new QLabel("These settings override profile defaults. They are intended for testing, benchmarking,"
                            " or expert tuning.",
                            content);
    note->setWordWrap(true);
    note->setStyleSheet("background: #1A2133; border-radius: 5px; padding: 10px 14px;"
                        " color: #8A9099; font-size: 12px;");
    layout->addWidget(note);

    // Effective settings summary
    layout->addWidget(makeSectionLabel("Effective Settings", content));
    auto* eff_frame = new QFrame(content);
    eff_frame->setStyleSheet("QFrame { background: #141A26; border-radius: 6px; }");
    auto* eff_layout = new QVBoxLayout(eff_frame);
    eff_layout->setContentsMargins(14, 10, 14, 10);
    eff_layout->setSpacing(4);
    for (const char* line : {
             "Container: MKV",
             "Video codec: AV1 (NVENC)",
             "Quality: Balanced  ·  CQ 24",
             "Frame rate: CFR 60 fps",
             "Resolution: Source",
             "Audio codec: Opus",
             "Cursor: Captured",
         }) {
        auto* lbl = new QLabel(line, eff_frame);
        lbl->setStyleSheet("color: #8A9099; font-size: 12px;");
        eff_layout->addWidget(lbl);
    }
    layout->addWidget(eff_frame);

    // Developer logging
    layout->addWidget(makeSectionLabel("Developer Logging Level", content));
    log_level_combo_ = new QComboBox(content);
    log_level_combo_->setMinimumWidth(200);
    log_level_combo_->setStyleSheet("QComboBox { background: #1A2133; border: 1px solid #2A3349; border-radius: 4px;"
                                    " padding: 6px 12px; color: #C8CBD0; }"
                                    "QComboBox::drop-down { border: none; }"
                                    "QComboBox QAbstractItemView { background: #1A2133; color: #C8CBD0;"
                                    " selection-background-color: #263050; border: 1px solid #2A3349; }");
    log_level_combo_->addItems({"Off", "Error", "Warning", "Info", "Debug", "Trace"});
    log_level_combo_->setCurrentIndex(3); // Info default
    layout->addWidget(log_level_combo_);

    // NVTX profiling
    layout->addWidget(makeSectionLabel("Profiling", content));
    nvtx_check_ = makeCheck("Enable NVTX / profiling markers", content);
    layout->addWidget(nvtx_check_);

    // Reset
    auto* reset_btn = new QPushButton("Reset Advanced Overrides", content);
    reset_btn->setStyleSheet("QPushButton { background: transparent; border: 1px solid #3A4254;"
                             " border-radius: 4px; padding: 7px 16px; color: #C0C4CC; }"
                             "QPushButton:hover { background: #1A2030; }");
    layout->addWidget(reset_btn);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(reset_btn, &QPushButton::clicked, this, &AdvancedPage::onReset);
}

void AdvancedPage::onReset() {
    log_level_combo_->setCurrentIndex(3); // Info
    nvtx_check_->setChecked(false);
}

} // namespace exosnap
