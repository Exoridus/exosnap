#include "AdvancedPage.h"

#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/ExoCheckBox.h"

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../ui/theme/ExoSnapMetrics.h"

namespace exosnap {

namespace {

QString ContainerLabel(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("MKV");
    case capability::Container::Mp4:
        return QStringLiteral("MP4");
    case capability::Container::WebM:
        return QStringLiteral("WebM");
    }
    return QStringLiteral("MKV");
}

QString VideoCodecLabel(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("H.264 (NVENC)");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("HEVC (NVENC)");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("AV1 (NVENC)");
    }
    return QStringLiteral("H.264 (NVENC)");
}

QString AudioCodecLabel(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("AAC");
    case capability::AudioCodec::Opus:
        return QStringLiteral("Opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("PCM");
    }
    return QStringLiteral("AAC");
}

QString QualityLabel(recorder_core::NvencQualityPreset quality) {
    switch (quality) {
    case recorder_core::NvencQualityPreset::High:
        return QStringLiteral("High  ·  CQ 19");
    case recorder_core::NvencQualityPreset::Balanced:
        return QStringLiteral("Balanced  ·  CQ 24");
    case recorder_core::NvencQualityPreset::Small:
        return QStringLiteral("Small  ·  CQ 30");
    }
    return QStringLiteral("Balanced  ·  CQ 24");
}

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

ui::widgets::ExoCheckBox* makeCheck(const QString& text, QWidget* parent) {
    return new ui::widgets::ExoCheckBox(text, parent);
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
        makeSubLabel("These values reflect the active profile and its resolved capture settings.", behavior_panel));

    auto* baseline_grid = new QFormLayout();
    baseline_grid->setContentsMargins(0, 0, 0, 0);
    baseline_grid->setHorizontalSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);
    baseline_grid->setVerticalSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);

    auto addBaselineRow = [&](const QString& key, QLabel*& value_label) {
        auto* key_label = new QLabel(key, behavior_panel);
        key_label->setProperty("labelRole", "subtle");
        value_label = new QLabel(QStringLiteral("—"), behavior_panel);
        value_label->setProperty("labelRole", "mono");
        baseline_grid->addRow(key_label, value_label);
    };

    addBaselineRow(QStringLiteral("Profile"), baseline_profile_label_);
    addBaselineRow(QStringLiteral("Container"), baseline_container_label_);
    addBaselineRow(QStringLiteral("Video codec"), baseline_video_label_);
    addBaselineRow(QStringLiteral("Quality"), baseline_quality_label_);
    addBaselineRow(QStringLiteral("Frame rate"), baseline_framerate_label_);
    addBaselineRow(QStringLiteral("Audio codec"), baseline_audio_label_);
    addBaselineRow(QStringLiteral("Cursor"), baseline_cursor_label_);
    behavior_layout->addLayout(baseline_grid);

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
    log_level_combo_->setMaximumWidth(280);
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

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(log_level_combo_);

    connect(reset_btn, &QPushButton::clicked, this, &AdvancedPage::onReset);
}

void AdvancedPage::setBaseline(const OutputSettingsModel& output, const VideoSettingsModel& video,
                               const QString& profile_name) {
    if (baseline_profile_label_)
        baseline_profile_label_->setText(profile_name.isEmpty() ? QStringLiteral("—") : profile_name);
    if (baseline_container_label_)
        baseline_container_label_->setText(ContainerLabel(output.container));
    if (baseline_video_label_)
        baseline_video_label_->setText(VideoCodecLabel(output.video_codec));
    if (baseline_quality_label_)
        baseline_quality_label_->setText(QualityLabel(video.quality));
    if (baseline_framerate_label_) {
        const QString mode = video.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR");
        baseline_framerate_label_->setText(QStringLiteral("%1 %2 fps").arg(mode).arg(video.frame_rate_num));
    }
    if (baseline_audio_label_)
        baseline_audio_label_->setText(AudioCodecLabel(output.audio_codec));
    if (baseline_cursor_label_)
        baseline_cursor_label_->setText(video.capture_cursor ? QStringLiteral("Captured") : QStringLiteral("Hidden"));
}

void AdvancedPage::onReset() {
    log_level_combo_->setCurrentIndex(3); // Info
    nvtx_check_->setChecked(false);
}

} // namespace exosnap
