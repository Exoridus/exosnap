#include "AdvancedPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
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

QCheckBox* makeCheck(const QString& text, QWidget* parent) {
    return new QCheckBox(text, parent);
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

} // namespace

AdvancedPage::AdvancedPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // Warning note
    auto* note = new QLabel("These settings override profile defaults. They are intended for testing, benchmarking,"
                            " or expert tuning.",
                            content);
    note->setWordWrap(true);
    note->setProperty("panelRole", "note");
    layout->addWidget(note);

    // Non-default behavior
    layout->addWidget(makeSectionLabel("Non-default Behavior", content));
    auto* behavior_panel = makePanel(content);
    auto* behavior_layout = new QVBoxLayout(behavior_panel);
    behavior_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    behavior_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);
    behavior_layout->addWidget(
        makeSubLabel("These values reflect the current resolved baseline profile.", behavior_panel));
    for (const char* line : {
             "Container: MKV",
             "Video codec: AV1 (NVENC)",
             "Quality: Balanced  ·  CQ 24",
             "Frame rate: CFR 60 fps",
             "Resolution: Source",
             "Audio codec: Opus",
             "Cursor: Captured",
         }) {
        auto* lbl = new QLabel(line, behavior_panel);
        lbl->setProperty("labelRole", "mono");
        behavior_layout->addWidget(lbl);
    }
    layout->addWidget(behavior_panel);

    // Developer / experimental controls
    layout->addWidget(makeSectionLabel("Developer / Experimental Controls", content));
    auto* controls_panel = makePanel(content);
    auto* controls_layout = new QVBoxLayout(controls_panel);
    controls_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    controls_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    controls_layout->addWidget(makeSectionLabel("Developer Logging Level", controls_panel));
    log_level_combo_ = new QComboBox(controls_panel);
    log_level_combo_->setMinimumWidth(200);
    log_level_combo_->addItems({"Off", "Error", "Warning", "Info", "Debug", "Trace"});
    log_level_combo_->setCurrentIndex(3); // Info default
    controls_layout->addWidget(log_level_combo_);

    // NVTX profiling
    controls_layout->addWidget(makeSectionLabel("Profiling", controls_panel));
    nvtx_check_ = makeCheck("Enable NVTX / profiling markers", controls_panel);
    controls_layout->addWidget(nvtx_check_);

    controls_layout->addWidget(makeSectionLabel("Safety", controls_panel));
    auto* reset_btn = new QPushButton("Reset Advanced Overrides", controls_panel);
    reset_btn->setProperty("role", "ghost");
    controls_layout->addWidget(reset_btn);
    layout->addWidget(controls_panel);

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
