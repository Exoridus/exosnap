#include "ConfigPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "../../../libs/recorder_core/include/recorder_core/audio_track_model.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputPathPolicy.h"
#include "../services/WebcamService.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/VUMeterWidget.h"
#include "../ui/widgets/WebcamSetupPanel.h"
#include "../viewmodels/PresentationStateBuilder.h"

#include <ctime>
#include <optional>

namespace exosnap {

namespace {

// Upper bound for the Config form width. Settings is a wide product surface;
// the cap prevents absurd stretching on ultra-wide displays while preserving
// the full two-column desktop rhythm at typical window sizes.
constexpr int kMaxContentWidth = 1440;

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

// Card title: 15/600 per the design system "Section/card title" role.
QLabel* makeCardTitle(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "cardTitle");
    return l;
}

// Mono uppercase "eyebrow" label that sits directly above a form control.
QLabel* makeFieldLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setProperty("labelRole", "fieldLabel");
    return l;
}

// Thin in-card divider, matching the prototype `.hr` rule.
QFrame* makeHRule(QWidget* parent) {
    auto* rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setProperty("frameRole", "sectionRuleLine");
    return rule;
}

QLabel* makeHint(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "muted");
    l->setWordWrap(true);
    return l;
}

QString VideoCodecLabel(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("H.264");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("HEVC");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("AV1");
    }
    return QStringLiteral("H.264");
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

int VideoCodecToInt(capability::VideoCodec codec) {
    return static_cast<int>(codec);
}

int AudioCodecToInt(capability::AudioCodec codec) {
    return static_cast<int>(codec);
}

capability::VideoCodec IntToVideoCodec(int value) {
    if (value == static_cast<int>(capability::VideoCodec::Av1Nvenc))
        return capability::VideoCodec::Av1Nvenc;
    if (value == static_cast<int>(capability::VideoCodec::HevcNvenc))
        return capability::VideoCodec::HevcNvenc;
    return capability::VideoCodec::H264Nvenc;
}

capability::AudioCodec IntToAudioCodec(int value) {
    if (value == static_cast<int>(capability::AudioCodec::Opus))
        return capability::AudioCodec::Opus;
    if (value == static_cast<int>(capability::AudioCodec::Pcm))
        return capability::AudioCodec::Pcm;
    return capability::AudioCodec::AacMf;
}

FilenameTargetContext ExamplePreviewContext(const QString& profile_name, const OutputSettingsModel& settings) {
    FilenameTargetContext context;
    context.target_name = L"Desktop - Display 1";
    context.app_name = L"Desktop";
    context.window_title = L"Display 1";
    context.process_name = L"desktop";
    context.profile_name = profile_name.toStdWString();
    context.video_codec = settings.video_codec;
    context.audio_codec = settings.audio_codec;
    return context;
}

} // namespace

ConfigPage::ConfigPage(const OutputSettingsModel& initial_settings, const VideoSettingsModel& initial_video,
                       QWidget* parent)
    : QWidget(parent), format_settings_(initial_settings), video_settings_(initial_video) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 36);
    layout->setSpacing(18);

    // ---- READINESS BANNER (full width) ----
    readiness_panel_ = makePanel(content);
    readiness_panel_->setProperty("panelRole", "readinessBanner");
    auto* status_layout = new QVBoxLayout(readiness_panel_);
    status_layout->setContentsMargins(18, 14, 18, 14);
    status_layout->setSpacing(6);

    auto* status_head = new QHBoxLayout();
    status_head->setSpacing(12);
    auto* status_text = new QVBoxLayout();
    status_text->setSpacing(2);

    readiness_badge_label_ = new QLabel(readiness_panel_);
    readiness_badge_label_->setProperty("labelRole", "cardTitle");
    status_text->addWidget(readiness_badge_label_);

    readiness_detail_label_ = new QLabel(readiness_panel_);
    readiness_detail_label_->setProperty("labelRole", "muted");
    readiness_detail_label_->setWordWrap(true);
    status_text->addWidget(readiness_detail_label_);

    status_head->addLayout(status_text, 1);

    view_details_btn_ = new QPushButton(QStringLiteral("Open Diagnostics..."), readiness_panel_);
    view_details_btn_->setProperty("role", "ghost");
    view_details_btn_->setVisible(false);
    status_head->addWidget(view_details_btn_, 0, Qt::AlignTop);
    status_layout->addLayout(status_head);

    lock_note_label_ = new QLabel(readiness_panel_);
    lock_note_label_->setObjectName(QStringLiteral("lockNoteLabel"));
    lock_note_label_->setProperty("labelRole", "muted");
    lock_note_label_->setWordWrap(true);
    lock_note_label_->setText(QStringLiteral("Recording settings are locked while recording."));
    lock_note_label_->setVisible(false);
    status_layout->addWidget(lock_note_label_);

    layout->addWidget(readiness_panel_);

    // ---- PRESET CARD (full width, top) ----
    // A preset is a full recording setup. The card exposes: selector, dirty indicator,
    // default badge, Save/Save As primary actions, and a "Manage" overflow menu.
    auto* preset_panel = makePanel(content);
    auto* preset_layout = new QVBoxLayout(preset_panel);
    preset_layout->setContentsMargins(18, 16, 18, 16);
    preset_layout->setSpacing(10);

    // Card title row: "Preset" title + dirty indicator + default badge.
    auto* preset_head = new QHBoxLayout();
    preset_head->setSpacing(8);
    preset_head->addWidget(makeCardTitle(QStringLiteral("Preset"), preset_panel));

    preset_dirty_indicator_ = new QLabel(preset_panel);
    preset_dirty_indicator_->setObjectName(QStringLiteral("presetDirtyIndicator"));
    preset_dirty_indicator_->setProperty("labelRole", "presetDirtyIndicator");
    preset_dirty_indicator_->setText(QStringLiteral("● Unsaved"));
    preset_dirty_indicator_->setVisible(false);
    preset_head->addWidget(preset_dirty_indicator_);

    preset_head->addStretch();

    preset_default_badge_ = new QLabel(preset_panel);
    preset_default_badge_->setObjectName(QStringLiteral("presetDefaultBadge"));
    preset_default_badge_->setProperty("labelRole", "profileStatusBadge");
    preset_default_badge_->setAlignment(Qt::AlignCenter);
    preset_default_badge_->setText(QStringLiteral("Default"));
    preset_default_badge_->setVisible(false);
    preset_head->addWidget(preset_default_badge_);

    profile_status_label_ = new QLabel(preset_panel);
    profile_status_label_->setProperty("labelRole", "profileStatusBadge");
    profile_status_label_->setAlignment(Qt::AlignCenter);
    profile_status_label_->setVisible(false);
    preset_head->addWidget(profile_status_label_);

    preset_layout->addLayout(preset_head);

    // Selector row: combo + Save + Save As + Manage menu.
    auto* profile_row = new QHBoxLayout();
    profile_row->setSpacing(8);
    profile_combo_ = new QComboBox(preset_panel);
    // Two stable objectNames: presetCombo (new contract) and profileCombo (existing tests).
    profile_combo_->setObjectName(QStringLiteral("profileCombo"));
    profile_combo_->setAccessibleName(QStringLiteral("presetCombo"));
    profile_combo_->setProperty("presetComboAlias", QStringLiteral("presetCombo"));
    profile_combo_->setMinimumWidth(220);
    profile_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Primary action buttons — Save (dirty-gated) and Save As (always available).
    preset_save_btn_ = new QPushButton(QStringLiteral("Save"), preset_panel);
    preset_save_btn_->setObjectName(QStringLiteral("presetSaveButton"));
    preset_save_btn_->setProperty("role", "ghost");
    preset_save_btn_->setEnabled(false);
    preset_save_btn_->setVisible(false);

    preset_save_as_btn_ = new QPushButton(QStringLiteral("Save As…"), preset_panel);
    preset_save_as_btn_->setObjectName(QStringLiteral("presetSaveAsButton"));
    preset_save_as_btn_->setProperty("role", "ghost");

    profile_overflow_btn_ = new QToolButton(preset_panel);
    profile_overflow_btn_->setObjectName(QStringLiteral("presetManageButton"));
    profile_overflow_btn_->setText(QStringLiteral("Manage presets"));
    profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
    profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* profile_menu = new QMenu(profile_overflow_btn_);
    // Section 1: Save actions.
    save_preset_action_ = profile_menu->addAction(QStringLiteral("Save preset"));
    save_preset_as_action_ = profile_menu->addAction(QStringLiteral("Save as new preset…"));
    profile_menu->addSeparator();
    // Section 2: Preset lifecycle.
    new_preset_action_ = profile_menu->addAction(QStringLiteral("New preset from default…"));
    duplicate_preset_action_ = profile_menu->addAction(QStringLiteral("Duplicate preset"));
    rename_preset_action_ = profile_menu->addAction(QStringLiteral("Rename preset…"));
    delete_preset_action_ = profile_menu->addAction(QStringLiteral("Delete preset"));
    profile_menu->addSeparator();
    // Section 3: Default assignment.
    set_default_preset_action_ = profile_menu->addAction(QStringLiteral("Set as default preset"));
    profile_menu->addSeparator();
    // Section 4: Reset — two CLEARLY SEPARATE actions.
    reset_changes_action_ = profile_menu->addAction(QStringLiteral("Reset changes"));
    profile_menu->addSeparator();
    // Destructive reset is separated so it cannot be confused with "Reset changes".
    reset_to_defaults_action_ = profile_menu->addAction(QStringLiteral("Reset all presets to factory defaults…"));
    profile_overflow_btn_->setMenu(profile_menu);

    profile_row->addWidget(profile_combo_, 1);
    profile_row->addWidget(preset_save_btn_);
    profile_row->addWidget(preset_save_as_btn_);
    profile_row->addWidget(profile_overflow_btn_);
    preset_layout->addLayout(profile_row);

    preset_layout->addWidget(
        makeHint(QStringLiteral("A preset stores the complete recording setup: source, video, audio, webcam, countdown "
                                "& output."),
                 preset_panel));
    layout->addWidget(preset_panel);

    // ---- TWO-COLUMN REGION (Format & encoding | Audio) ----
    // The left column holds Format & encoding; the right column holds Audio. On narrow
    // viewports the columns flip from side-by-side to a single stacked column
    // (updateResponsiveLayout()).
    auto* columns = new QWidget(content);
    columns_layout_ = new QHBoxLayout(columns);
    columns_layout_->setContentsMargins(0, 0, 0, 0);
    columns_layout_->setSpacing(18);

    auto* left_col = new QWidget(columns);
    auto* left_layout = new QVBoxLayout(left_col);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(18);

    auto* right_col = new QWidget(columns);
    auto* right_layout = new QVBoxLayout(right_col);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(18);

    columns_layout_->addWidget(left_col, 1);
    columns_layout_->addWidget(right_col, 1);
    layout->addWidget(columns);

    // ---- FORMAT & ENCODING CARD (left) ----
    // Container, codecs, quality, frame rate, timing, and cursor live together here.
    auto* fmt_panel = makePanel(left_col);
    auto* fmt_layout = new QVBoxLayout(fmt_panel);
    fmt_layout->setContentsMargins(18, 16, 18, 18);
    fmt_layout->setSpacing(12);
    fmt_layout->addWidget(makeCardTitle(QStringLiteral("Format & encoding"), fmt_panel));

    format_display_label_ = new QLabel(fmt_panel);
    format_display_label_->setProperty("labelRole", "muted");
    fmt_layout->addWidget(format_display_label_);

    fmt_layout->addWidget(makeFieldLabel(QStringLiteral("Container"), fmt_panel));
    container_group_ = new QButtonGroup(this);
    auto* container_row = new QHBoxLayout();
    container_row->setSpacing(18);
    mkv_radio_ = new QRadioButton(QStringLiteral("MKV"), fmt_panel);
    webm_radio_ = new QRadioButton(QStringLiteral("WebM"), fmt_panel);
    mp4_radio_ = new QRadioButton(QStringLiteral("MP4"), fmt_panel);
    container_group_->addButton(mkv_radio_, static_cast<int>(capability::Container::Matroska));
    container_group_->addButton(webm_radio_, static_cast<int>(capability::Container::WebM));
    container_group_->addButton(mp4_radio_, static_cast<int>(capability::Container::Mp4));
    container_row->addWidget(mkv_radio_);
    container_row->addWidget(webm_radio_);
    container_row->addWidget(mp4_radio_);
    container_row->addStretch();
    fmt_layout->addLayout(container_row);

    auto* codec_row = new QHBoxLayout();
    codec_row->setSpacing(14);

    auto* vcol = new QVBoxLayout();
    vcol->setSpacing(6);
    video_codec_combo_ = new QComboBox(fmt_panel);
    vcol->addWidget(makeFieldLabel(QStringLiteral("Video codec"), fmt_panel));
    vcol->addWidget(video_codec_combo_);
    codec_row->addLayout(vcol, 1);

    auto* acol = new QVBoxLayout();
    acol->setSpacing(6);
    audio_codec_combo_ = new QComboBox(fmt_panel);
    acol->addWidget(makeFieldLabel(QStringLiteral("Audio codec"), fmt_panel));
    acol->addWidget(audio_codec_combo_);
    codec_row->addLayout(acol, 1);

    fmt_layout->addLayout(codec_row);

    // Quality — compact 3-segment control. The hidden videoQualityCombo stays the single
    // place that emits the model change, so the existing summary flow and test seam are kept.
    fmt_layout->addWidget(makeFieldLabel(QStringLiteral("Quality"), fmt_panel));
    quality_combo_ = new QComboBox(fmt_panel);
    quality_combo_->setObjectName(QStringLiteral("videoQualityCombo"));
    quality_combo_->addItem(QStringLiteral("High Quality"), static_cast<int>(recorder_core::NvencQualityPreset::High));
    quality_combo_->addItem(QStringLiteral("Balanced"), static_cast<int>(recorder_core::NvencQualityPreset::Balanced));
    quality_combo_->addItem(QStringLiteral("Small"), static_cast<int>(recorder_core::NvencQualityPreset::Small));
    quality_combo_->setVisible(false);
    quality_combo_->setFocusPolicy(Qt::NoFocus);
    fmt_layout->addWidget(quality_combo_);

    auto* quality_segmented = new QWidget(fmt_panel);
    quality_segmented->setObjectName(QStringLiteral("qualitySegmented"));
    auto* quality_segmented_layout = new QHBoxLayout(quality_segmented);
    quality_segmented_layout->setContentsMargins(3, 3, 3, 3);
    quality_segmented_layout->setSpacing(0);

    quality_segment_group_ = new QButtonGroup(this);
    quality_segment_group_->setExclusive(true);

    auto makeQualitySegment = [&](const QString& object_name, const QString& label,
                                  recorder_core::NvencQualityPreset preset) -> QPushButton* {
        auto* segment = new QPushButton(label, quality_segmented);
        segment->setObjectName(object_name);
        segment->setCheckable(true);
        segment->setAutoDefault(false);
        segment->setDefault(false);
        segment->setCursor(Qt::PointingHandCursor);
        segment->setProperty("qualitySegment", true);
        segment->setProperty("qualitySegmentSelected", false);
        segment->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        quality_segment_group_->addButton(segment, static_cast<int>(preset));
        quality_segmented_layout->addWidget(segment);
        return segment;
    };

    quality_segment_small_ = makeQualitySegment(QStringLiteral("qualitySegmentSmall"), QStringLiteral("Small · CQ30"),
                                                recorder_core::NvencQualityPreset::Small);
    quality_segment_balanced_ =
        makeQualitySegment(QStringLiteral("qualitySegmentBalanced"), QStringLiteral("Balanced · CQ24"),
                           recorder_core::NvencQualityPreset::Balanced);
    quality_segment_high_ = makeQualitySegment(QStringLiteral("qualitySegmentHigh"), QStringLiteral("High · CQ19"),
                                               recorder_core::NvencQualityPreset::High);

    fmt_layout->addWidget(quality_segmented);

    quality_badge_label_ = new QLabel(fmt_panel);
    quality_badge_label_->setObjectName(QStringLiteral("qualityBadgeLabel"));
    quality_badge_label_->setProperty("labelRole", "muted");
    fmt_layout->addWidget(quality_badge_label_);

    quality_settings_label_ = new QLabel(fmt_panel);
    quality_settings_label_->setObjectName(QStringLiteral("qualitySettingsLabel"));
    quality_settings_label_->setProperty("labelRole", "muted");
    fmt_layout->addWidget(quality_settings_label_);

    // Frame rate + timing. The recording pipeline is CFR-60-first and the frame rate is not
    // user-selectable in this build, so it is shown read-only rather than as a fake selector.
    auto* rate_row = new QHBoxLayout();
    rate_row->setSpacing(14);
    auto* rate_col = new QVBoxLayout();
    rate_col->setSpacing(6);
    rate_col->addWidget(makeFieldLabel(QStringLiteral("Frame rate"), fmt_panel));
    frame_rate_combo_ = new QComboBox(fmt_panel);
    frame_rate_combo_->setObjectName(QStringLiteral("frameRateCombo"));
    frame_rate_combo_->addItem(QStringLiteral("60 fps"));
    frame_rate_combo_->setEnabled(false);
    frame_rate_combo_->setToolTip(QStringLiteral("Frame rate is fixed at 60 fps in this build."));
    rate_col->addWidget(frame_rate_combo_);
    rate_row->addLayout(rate_col, 1);
    rate_row->addStretch(1);
    fmt_layout->addLayout(rate_row);

    cfr_check_ = new QCheckBox(QStringLiteral("Constant frame rate (CFR)"), fmt_panel);
    cfr_check_->setChecked(video_settings_.cfr);
    fmt_layout->addWidget(cfr_check_);

    cursor_check_ = new QCheckBox(QStringLiteral("Capture cursor"), fmt_panel);
    cursor_check_->setChecked(video_settings_.capture_cursor);
    fmt_layout->addWidget(cursor_check_);

    fmt_layout->addWidget(makeHint(
        QStringLiteral("VFR can desync audio in some editors — keep CFR on unless you know you need otherwise."),
        fmt_panel));

    left_layout->addWidget(fmt_panel);
    left_layout->addStretch();

    // ---- AUDIO CARD (right) ----
    auto* audio_panel = makePanel(right_col);
    auto* audio_panel_layout = new QVBoxLayout(audio_panel);
    audio_panel_layout->setContentsMargins(18, 16, 18, 18);
    audio_panel_layout->setSpacing(10);
    audio_panel_layout->addWidget(makeCardTitle(QStringLiteral("Audio"), audio_panel));

    // Helper: build a source row directly into a given layout+parent.
    auto makeSourceRowInto = [&](QVBoxLayout* target_layout, QWidget* target_parent, const QString& title,
                                 QCheckBox*& enabled_check, QCheckBox*& separate_check, QLabel*& source_label,
                                 ui::widgets::VUMeterWidget*& meter_out, QLabel*& db_label_out) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);

        enabled_check = new QCheckBox(title, target_parent);
        separate_check = new QCheckBox(QStringLiteral("Separate track"), target_parent);

        db_label_out = new QLabel(QStringLiteral("–"), target_parent);
        db_label_out->setProperty("labelRole", "muted");
        db_label_out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        db_label_out->setMinimumWidth(52);

        row->addWidget(enabled_check);
        row->addStretch();
        row->addWidget(db_label_out);
        row->addWidget(separate_check);
        target_layout->addLayout(row);

        meter_out = new ui::widgets::VUMeterWidget(target_parent);
        meter_out->setActive(false);
        target_layout->addWidget(meter_out);

        source_label = new QLabel(target_parent);
        source_label->setProperty("labelRole", "muted");
        source_label->setWordWrap(true);
        target_layout->addWidget(source_label);
    };

    // System audio row (label + description change based on capture target kind):
    //   Display/Region → "Computer audio"
    //   Window        → "Other system audio"
    makeSourceRowInto(audio_panel_layout, audio_panel, QStringLiteral("Computer audio"), sys_enabled_check_,
                      sys_separate_check_, sys_source_label_, audio_sys_meter_, audio_sys_db_label_);
    sys_enabled_check_->setObjectName(QStringLiteral("settingsAudioSysCheck"));
    audio_sys_meter_->setObjectName(QStringLiteral("settingsAudioSysMeter"));
    audio_sys_db_label_->setObjectName(QStringLiteral("settingsAudioSysDbLabel"));

    // Application audio section — wrapped in a container widget that is shown
    // for Window targets and hidden for Display/Region targets.
    app_row_section_ = new QWidget(audio_panel);
    app_row_section_->setObjectName(QStringLiteral("settingsAudioAppSection"));
    {
        auto* app_section_layout = new QVBoxLayout(app_row_section_);
        app_section_layout->setContentsMargins(0, 0, 0, 0);
        app_section_layout->setSpacing(audio_panel_layout->spacing());

        auto* app_rule = new QFrame(app_row_section_);
        app_rule->setFrameShape(QFrame::HLine);
        app_rule->setProperty("frameRole", "sectionRuleLine");
        app_section_layout->addWidget(app_rule);

        makeSourceRowInto(app_section_layout, app_row_section_, QStringLiteral("Application audio"), app_enabled_check_,
                          app_separate_check_, app_source_label_, audio_app_meter_, audio_app_db_label_);
        app_enabled_check_->setObjectName(QStringLiteral("settingsAudioAppCheck"));
        audio_app_meter_->setObjectName(QStringLiteral("settingsAudioAppMeter"));
        audio_app_db_label_->setObjectName(QStringLiteral("settingsAudioAppDbLabel"));
    }
    audio_panel_layout->addWidget(app_row_section_);
    // Hidden by default — shown when target kind is Window.
    app_row_section_->setVisible(false);

    audio_panel_layout->addWidget(makeHRule(audio_panel));
    makeSourceRowInto(audio_panel_layout, audio_panel, QStringLiteral("Microphone"), mic_enabled_check_,
                      mic_separate_check_, mic_source_label_, audio_mic_meter_, audio_mic_db_label_);
    audio_mic_meter_->setObjectName(QStringLiteral("settingsAudioMicMeter"));
    audio_mic_db_label_->setObjectName(QStringLiteral("settingsAudioMicDbLabel"));

    mic_device_combo_ = new QComboBox(audio_panel);
    mic_device_combo_->setObjectName(QStringLiteral("micDeviceCombo"));
    mic_device_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mic_device_combo_->setMinimumWidth(180);
    audio_panel_layout->addWidget(mic_device_combo_);

    audio_panel_layout->addWidget(
        makeHint(QStringLiteral("Separate tracks keep each source on its own channel for editing."), audio_panel));

    audio_summary_label_ = new QLabel(audio_panel);
    audio_summary_label_->setProperty("labelRole", "muted");
    audio_summary_label_->setWordWrap(true);
    audio_summary_label_->setVisible(false);
    audio_panel_layout->addWidget(audio_summary_label_);
    right_layout->addWidget(audio_panel);
    right_layout->addStretch();

    // ---- WEBCAM CARD (full width — inline setup panel, no navigation required) ----
    auto* webcam_panel = makePanel(content);
    auto* webcam_panel_layout = new QVBoxLayout(webcam_panel);
    webcam_panel_layout->setContentsMargins(18, 16, 18, 18);
    webcam_panel_layout->setSpacing(10);
    webcam_panel_layout->addWidget(makeCardTitle(QStringLiteral("Webcam"), webcam_panel));

    webcam_setup_panel_ = new ui::widgets::WebcamSetupPanel(webcam_panel);
    webcam_setup_panel_->setObjectName(QStringLiteral("settingsWebcamSetupPanel"));
    webcam_panel_layout->addWidget(webcam_setup_panel_);
    layout->addWidget(webcam_panel);

    // ---- OUTPUT CARD (full width) ----
    auto* out_panel = makePanel(content);
    auto* out_panel_layout = new QVBoxLayout(out_panel);
    out_panel_layout->setContentsMargins(18, 16, 18, 18);
    out_panel_layout->setSpacing(12);
    out_panel_layout->addWidget(makeCardTitle(QStringLiteral("Output"), out_panel));

    // Output resolution: scaling is not implemented in this build, so the control is shown
    // as a disabled/planned segmented fixed at the source ("Native") — never a fake scaler.
    out_panel_layout->addWidget(makeFieldLabel(QStringLiteral("Output resolution"), out_panel));
    auto* out_res_segmented = new QWidget(out_panel);
    out_res_segmented->setObjectName(QStringLiteral("outputResSegmented"));
    auto* out_res_layout = new QHBoxLayout(out_res_segmented);
    out_res_layout->setContentsMargins(3, 3, 3, 3);
    out_res_layout->setSpacing(0);
    for (const QString& opt : {QStringLiteral("Native"), QStringLiteral("4K"), QStringLiteral("1440p"),
                               QStringLiteral("1080p"), QStringLiteral("720p")}) {
        auto* seg = new QPushButton(opt, out_res_segmented);
        const bool is_native = opt == QStringLiteral("Native");
        seg->setCheckable(true);
        seg->setChecked(is_native);
        seg->setProperty("qualitySegment", true);
        seg->setProperty("qualitySegmentSelected", is_native);
        seg->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        seg->setEnabled(false);
        out_res_layout->addWidget(seg);
    }
    out_panel_layout->addWidget(out_res_segmented);
    out_panel_layout->addWidget(makeHint(
        QStringLiteral("Output scaling is planned — recordings currently use the source resolution."), out_panel));

    auto* output_split = new QWidget(out_panel);
    output_split_layout_ = new QHBoxLayout(output_split);
    output_split_layout_->setContentsMargins(0, 0, 0, 0);
    output_split_layout_->setSpacing(24);

    auto* output_fields = new QWidget(output_split);
    auto* output_fields_layout = new QVBoxLayout(output_fields);
    output_fields_layout->setContentsMargins(0, 0, 0, 0);
    output_fields_layout->setSpacing(8);

    output_fields_layout->addWidget(makeFieldLabel(QStringLiteral("Destination folder"), output_fields));
    auto* dest_row = new QHBoxLayout();
    dest_row->setSpacing(8);
    destination_edit_ = new QLineEdit(output_fields);
    destination_edit_->setObjectName(QStringLiteral("destinationEdit"));
    destination_edit_->setPlaceholderText(QString::fromStdWString(format_settings_.output_folder.wstring()));
    browse_btn_ = new QPushButton(QStringLiteral("Browse..."), output_fields);
    browse_btn_->setProperty("role", "ghost");
    dest_row->addWidget(destination_edit_, 1);
    dest_row->addWidget(browse_btn_);
    output_fields_layout->addLayout(dest_row);

    folder_validation_label_ = makeHint(QString(), output_fields);
    folder_validation_label_->setVisible(false);
    output_fields_layout->addWidget(folder_validation_label_);

    output_fields_layout->addWidget(makeFieldLabel(QStringLiteral("Filename pattern"), output_fields));
    naming_edit_ = new QLineEdit(output_fields);
    naming_edit_->setObjectName(QStringLiteral("namingEdit"));
    naming_edit_->setPlaceholderText(QStringLiteral("{datetime}_{app}_{title}"));
    output_fields_layout->addWidget(naming_edit_);

    pattern_validation_label_ = makeHint(QString(), output_fields);
    pattern_validation_label_->setVisible(false);
    output_fields_layout->addWidget(pattern_validation_label_);

    example_filename_label_ = makeHint(QString(), output_fields);
    output_fields_layout->addWidget(example_filename_label_);
    output_fields_layout->addStretch();

    auto* output_help = new QWidget(output_split);
    auto* output_help_layout = new QVBoxLayout(output_help);
    output_help_layout->setContentsMargins(0, 0, 0, 0);
    output_help_layout->setSpacing(8);

    output_help_layout->addWidget(makeFieldLabel(QStringLiteral("Filename tokens"), output_help));

    // Compact chips of the most common real tokens; the full reference stays behind the toggle.
    // Only tokens the FilenameBuilder actually resolves are shown (e.g. {target}/{profile}).
    const QStringList token_chips = {QStringLiteral("{datetime}"), QStringLiteral("{date}"),
                                     QStringLiteral("{time}"),     QStringLiteral("{app}"),
                                     QStringLiteral("{title}"),    QStringLiteral("{target}"),
                                     QStringLiteral("{profile}"),  QStringLiteral("{container}")};
    QHBoxLayout* chip_row = nullptr;
    for (int i = 0; i < token_chips.size(); ++i) {
        if (i % 4 == 0) {
            chip_row = new QHBoxLayout();
            chip_row->setContentsMargins(0, 0, 0, 0);
            chip_row->setSpacing(6);
            output_help_layout->addLayout(chip_row);
        }
        auto* chip = new QLabel(token_chips.at(i), output_help);
        chip->setProperty("labelRole", "tokenChip");
        chip_row->addWidget(chip, 0, Qt::AlignLeft);
    }
    if (chip_row)
        chip_row->addStretch();

    token_help_toggle_btn_ = new QPushButton(QStringLiteral("Show token reference"), output_help);
    token_help_toggle_btn_->setObjectName(QStringLiteral("tokenHelpToggle"));
    token_help_toggle_btn_->setProperty("role", "ghost");
    output_help_layout->addWidget(token_help_toggle_btn_, 0, Qt::AlignLeft);

    token_help_label_ = makeHint(
        QStringLiteral("Tokens: {datetime}, {date}, {time}, {timestamp}, {YYYY}, {YY}, {MM}, {DD}, {hh}, {mm}, {ss}, "
                       "{app}, {title}, {process}, {target}, {profile}, {container}, {video}, {audio}"),
        output_help);
    token_help_label_->setVisible(false);
    output_help_layout->addWidget(token_help_label_);

    output_help_layout->addWidget(
        makeHint(QStringLiteral("Tokens auto-fill names from the date, app, and capture target."), output_help));
    output_help_layout->addStretch();

    output_split_layout_->addWidget(output_fields, 3);
    output_split_layout_->addWidget(output_help, 2);
    out_panel_layout->addWidget(output_split);
    layout->addWidget(out_panel);

    // ---- ADVANCED SUMMARY (full width) ----
    auto* advanced_panel = makePanel(content);
    advanced_panel->setProperty("panelRole", "note");
    auto* advanced_layout = new QVBoxLayout(advanced_panel);
    advanced_layout->setContentsMargins(18, 14, 18, 14);
    advanced_layout->setSpacing(8);

    auto* advanced_head = new QHBoxLayout();
    advanced_head->setSpacing(12);
    auto* advanced_text = new QVBoxLayout();
    advanced_text->setSpacing(2);
    advanced_text->addWidget(makeCardTitle(QStringLiteral("Advanced / Expert Settings"), advanced_panel));
    advanced_text->addWidget(makeHint(
        QStringLiteral("Normal recording configuration lives in Settings. Use Advanced for diagnostics, developer, "
                       "and expert-only options."),
        advanced_panel));
    advanced_head->addLayout(advanced_text, 1);

    auto* advanced_open_btn = new QPushButton(QStringLiteral("Open Advanced"), advanced_panel);
    advanced_open_btn->setObjectName(QStringLiteral("advancedDetailsBtn"));
    advanced_open_btn->setProperty("role", "ghost");
    advanced_head->addWidget(advanced_open_btn, 0, Qt::AlignVCenter);
    advanced_layout->addLayout(advanced_head);
    layout->addWidget(advanced_panel);

    layout->addStretch();

    content->setMaximumWidth(kMaxContentWidth);
    {
        auto* centering_host = new QWidget();
        auto* ch = new QHBoxLayout(centering_host);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->addStretch(1);
        ch->addWidget(content, 0);
        ch->addStretch(1);
        scroll->setWidget(centering_host);
    }
    outer->addWidget(scroll);

    connect(advanced_open_btn, &QPushButton::clicked, this, &ConfigPage::advancedRequested);

    connect(container_group_, &QButtonGroup::idClicked, this, &ConfigPage::onContainerChanged);
    connect(video_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoCodecChanged);
    connect(audio_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onAudioCodecChanged);
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onProfileSelectionChanged);
    connect(quality_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConfigPage::onQualityChanged);
    connect(quality_segment_group_, &QButtonGroup::idClicked, this, &ConfigPage::onQualitySegmentSelected);
    connect(cfr_check_, &QCheckBox::toggled, this, &ConfigPage::onCfrChanged);
    connect(cursor_check_, &QCheckBox::toggled, this, &ConfigPage::onCursorChanged);
    connect(browse_btn_, &QPushButton::clicked, this, &ConfigPage::onBrowse);
    connect(destination_edit_, &QLineEdit::editingFinished, this, &ConfigPage::onDestinationEditingFinished);
    connect(naming_edit_, &QLineEdit::editingFinished, this, &ConfigPage::onPatternEditingFinished);
    connect(app_enabled_check_, &QCheckBox::toggled, this, &ConfigPage::onAudioAppToggled);
    connect(mic_enabled_check_, &QCheckBox::toggled, this, &ConfigPage::onAudioMicToggled);
    connect(sys_enabled_check_, &QCheckBox::toggled, this, &ConfigPage::onAudioSysToggled);
    connect(app_separate_check_, &QCheckBox::toggled, this, &ConfigPage::onAudioAppSeparateToggled);
    connect(mic_separate_check_, &QCheckBox::toggled, this, &ConfigPage::onAudioMicSeparateToggled);
    connect(sys_separate_check_, &QCheckBox::toggled, this, &ConfigPage::onAudioSysSeparateToggled);
    connect(mic_device_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onMicDeviceChanged);
    connect(webcam_setup_panel_, &ui::widgets::WebcamSetupPanel::settingsChanged, this,
            [this](const WebcamSettings& settings) {
                webcam_settings_ = settings;
                emit webcamSettingsChanged(webcam_settings_);
            });
    // Preset management connections — overflow menu.
    connect(save_preset_action_, &QAction::triggered, this, &ConfigPage::onSavePreset);
    connect(save_preset_as_action_, &QAction::triggered, this, &ConfigPage::onSavePresetAs);
    connect(new_preset_action_, &QAction::triggered, this, &ConfigPage::onNewPreset);
    connect(duplicate_preset_action_, &QAction::triggered, this, &ConfigPage::onDuplicatePreset);
    connect(rename_preset_action_, &QAction::triggered, this, &ConfigPage::onRenamePreset);
    connect(delete_preset_action_, &QAction::triggered, this, &ConfigPage::onDeletePreset);
    connect(set_default_preset_action_, &QAction::triggered, this, &ConfigPage::onSetDefaultPreset);
    connect(reset_changes_action_, &QAction::triggered, this, &ConfigPage::onResetChanges);
    connect(reset_to_defaults_action_, &QAction::triggered, this, &ConfigPage::onResetToDefaults);
    // Preset management connections — primary action buttons.
    connect(preset_save_btn_, &QPushButton::clicked, this, &ConfigPage::onSavePreset);
    connect(preset_save_as_btn_, &QPushButton::clicked, this, &ConfigPage::onSavePresetAs);
    connect(view_details_btn_, &QPushButton::clicked, this, &ConfigPage::diagnosticsRequested);
    connect(token_help_toggle_btn_, &QPushButton::clicked, this, [this]() {
        const bool now_visible = !token_help_label_->isVisible();
        token_help_label_->setVisible(now_visible);
        token_help_toggle_btn_->setText(now_visible ? QStringLiteral("Hide token reference")
                                                    : QStringLiteral("Show token reference"));
    });

    // Prevent accidental value changes when the mouse wheel scrolls the (long) Config
    // page while the cursor happens to be over a combo box. The filter forwards the
    // wheel event to the scroll area instead of changing the combo selection.
    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(profile_combo_);
    combo_wheel_filter->installOn(video_codec_combo_);
    combo_wheel_filter->installOn(audio_codec_combo_);
    combo_wheel_filter->installOn(quality_combo_);
    combo_wheel_filter->installOn(mic_device_combo_);

    setReadinessStatus(QStringLiteral("CHECKING"));

    {
        const QSignalBlocker dd(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(format_settings_.output_folder.wstring()));
    }
    {
        const QSignalBlocker np(naming_edit_);
        naming_edit_->setText(QString::fromStdWString(format_settings_.naming_pattern));
    }
    applyAudioConfigurationState();
    updateFormatDisplay();
    updateExampleFilename();
    updateQualitySummary();
    updateResponsiveLayout();

    QPointer<ConfigPage> safe = this;
    QTimer::singleShot(0, this, [safe]() {
        if (safe)
            safe->refreshMicDevices();
    });
}

void ConfigPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void ConfigPage::updateResponsiveLayout() {
    // Below this width the two-column form (and the Output card's field/help split)
    // becomes too cramped, so flip both to a single stacked column.
    const bool narrow = width() < 880;
    const QBoxLayout::Direction desired = narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;

    if (columns_layout_ && columns_layout_->direction() != desired)
        columns_layout_->setDirection(desired);
    if (output_split_layout_ && output_split_layout_->direction() != desired)
        output_split_layout_->setDirection(desired);
}

void ConfigPage::emitCurrentFormatSettings() {
    if (destination_edit_) {
        const auto folder_normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
        if (folder_normalized.result == OutputFolderPolicyResult::Ok) {
            format_settings_.output_folder = folder_normalized.resolved_path;
        }
    }
    if (naming_edit_) {
        const auto pattern_normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
        if (pattern_normalized.result == FilenamePatternPolicyResult::Ok) {
            format_settings_.naming_pattern = pattern_normalized.normalized_pattern;
        }
    }
    reconcileContainerCodecRules();
    updateFormatDisplay();
    updateOutputValidationState();
    updateExampleFilename();
    emit formatSettingsChanged(format_settings_);
}

void ConfigPage::emitCurrentVideoSettings() {
    emit videoSettingsChanged(video_settings_);
}

void ConfigPage::onQualityChanged(int index) {
    if (index < 0)
        return;
    video_settings_.quality = static_cast<recorder_core::NvencQualityPreset>(quality_combo_->itemData(index).toInt());
    updateQualitySummary();
    emitCurrentVideoSettings();
}

void ConfigPage::onQualitySegmentSelected(int preset_id) {
    if (!quality_combo_)
        return;

    const int idx = quality_combo_->findData(preset_id);
    if (idx < 0)
        return;
    if (quality_combo_->currentIndex() == idx) {
        updateQualitySegmentSelection();
        return;
    }
    quality_combo_->setCurrentIndex(idx);
}

void ConfigPage::onCfrChanged() {
    video_settings_.cfr = cfr_check_->isChecked();
    updateQualitySummary();
    emitCurrentVideoSettings();
}

void ConfigPage::onCursorChanged() {
    video_settings_.capture_cursor = cursor_check_->isChecked();
    updateQualitySummary();
    emitCurrentVideoSettings();
}

void ConfigPage::reconcileContainerCodecRules() {
    if (format_settings_.container == capability::Container::Mp4) {
        format_settings_.video_codec = capability::VideoCodec::H264Nvenc;
        format_settings_.audio_codec = capability::AudioCodec::AacMf;
        updateVideoCodecChoices();
        updateAudioCodecChoices();
        return;
    }
    if (format_settings_.container == capability::Container::WebM) {
        format_settings_.video_codec = capability::VideoCodec::Av1Nvenc;
        format_settings_.audio_codec = capability::AudioCodec::Opus;
        updateVideoCodecChoices();
        updateAudioCodecChoices();
        return;
    }
    if (format_settings_.video_codec == capability::VideoCodec::HevcNvenc) {
        format_settings_.video_codec = capability::VideoCodec::H264Nvenc;
    }
    if (format_settings_.video_codec == capability::VideoCodec::H264Nvenc &&
        format_settings_.audio_codec == capability::AudioCodec::Opus) {
        format_settings_.audio_codec = capability::AudioCodec::AacMf;
    }
    if (format_settings_.audio_codec == capability::AudioCodec::Pcm) {
        format_settings_.audio_codec = capability::AudioCodec::AacMf;
    }
    updateVideoCodecChoices();
    updateAudioCodecChoices();
}

void ConfigPage::updateVideoCodecChoices() {
    const QSignalBlocker blocker(video_codec_combo_);
    video_codec_combo_->clear();
    if (format_settings_.container == capability::Container::Mp4) {
        video_codec_combo_->addItem(QStringLiteral("H.264"), VideoCodecToInt(capability::VideoCodec::H264Nvenc));
    } else if (format_settings_.container == capability::Container::WebM) {
        video_codec_combo_->addItem(QStringLiteral("AV1"), VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
    } else {
        video_codec_combo_->addItem(QStringLiteral("H.264"), VideoCodecToInt(capability::VideoCodec::H264Nvenc));
        video_codec_combo_->addItem(QStringLiteral("AV1"), VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
    }
    const int vidx = video_codec_combo_->findData(VideoCodecToInt(format_settings_.video_codec));
    if (vidx >= 0)
        video_codec_combo_->setCurrentIndex(vidx);
}

void ConfigPage::updateAudioCodecChoices() {
    const QSignalBlocker blocker(audio_codec_combo_);
    audio_codec_combo_->clear();
    if (format_settings_.container == capability::Container::Mp4) {
        audio_codec_combo_->addItem(QStringLiteral("AAC"), AudioCodecToInt(capability::AudioCodec::AacMf));
    } else if (format_settings_.container == capability::Container::WebM) {
        audio_codec_combo_->addItem(QStringLiteral("Opus"), AudioCodecToInt(capability::AudioCodec::Opus));
    } else if (format_settings_.video_codec == capability::VideoCodec::Av1Nvenc) {
        audio_codec_combo_->addItem(QStringLiteral("Opus"), AudioCodecToInt(capability::AudioCodec::Opus));
        audio_codec_combo_->addItem(QStringLiteral("AAC"), AudioCodecToInt(capability::AudioCodec::AacMf));
    } else {
        audio_codec_combo_->addItem(QStringLiteral("AAC"), AudioCodecToInt(capability::AudioCodec::AacMf));
    }
    const int aidx = audio_codec_combo_->findData(AudioCodecToInt(format_settings_.audio_codec));
    if (aidx >= 0)
        audio_codec_combo_->setCurrentIndex(aidx);
}

void ConfigPage::updateFormatDisplay() {
    const QString summary = ContainerLabel(format_settings_.container) + QStringLiteral(" · ") +
                            VideoCodecLabel(format_settings_.video_codec) + QStringLiteral(" · ") +
                            AudioCodecLabel(format_settings_.audio_codec);
    format_display_label_->setText(QStringLiteral("Current format: ") + summary);
}

void ConfigPage::onContainerChanged(int id) {
    format_settings_.container = static_cast<capability::Container>(id);
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoCodecChanged(int index) {
    if (index < 0)
        return;
    format_settings_.video_codec = IntToVideoCodec(video_codec_combo_->itemData(index).toInt());
    emitCurrentFormatSettings();
}

void ConfigPage::onAudioCodecChanged(int index) {
    if (index < 0)
        return;
    format_settings_.audio_codec = IntToAudioCodec(audio_codec_combo_->itemData(index).toInt());
    emitCurrentFormatSettings();
}

void ConfigPage::onProfileSelectionChanged(int index) {
    if (index < 0 || index >= static_cast<int>(profile_options_.size()))
        return;
    const auto& opt = profile_options_[static_cast<std::size_t>(index)];
    active_preset_is_built_in_ = opt.built_in;
    active_preset_is_available_ = opt.available;
    active_preset_id_ = opt.id;
    updatePresetActionState();
    emit presetSelected(opt.id);
}

void ConfigPage::setOutputSettings(const OutputSettingsModel& settings) {
    format_settings_.container = settings.container;
    format_settings_.video_codec = settings.video_codec;
    format_settings_.audio_codec = settings.audio_codec;
    format_settings_.output_folder = settings.output_folder;
    format_settings_.naming_pattern = settings.naming_pattern;

    const QSignalBlocker blocker(container_group_);
    if (settings.container == capability::Container::Matroska)
        mkv_radio_->setChecked(true);
    else if (settings.container == capability::Container::WebM)
        webm_radio_->setChecked(true);
    else
        mp4_radio_->setChecked(true);

    updateVideoCodecChoices();
    updateAudioCodecChoices();
    updateFormatDisplay();

    if (destination_edit_) {
        const QSignalBlocker db(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(settings.output_folder.wstring()));
    }
    if (naming_edit_) {
        const QSignalBlocker nb(naming_edit_);
        naming_edit_->setText(QString::fromStdWString(settings.naming_pattern));
    }
    updateOutputValidationState();
    updateExampleFilename();
}

void ConfigPage::setVideoSettings(const VideoSettingsModel& settings) {
    video_settings_ = settings;

    const QSignalBlocker qb(quality_combo_);
    const int qidx = quality_combo_->findData(static_cast<int>(settings.quality));
    if (qidx >= 0)
        quality_combo_->setCurrentIndex(qidx);

    const QSignalBlocker cb(cfr_check_);
    cfr_check_->setChecked(settings.cfr);

    const QSignalBlocker crb(cursor_check_);
    cursor_check_->setChecked(settings.capture_cursor);

    updateQualitySummary();
}

void ConfigPage::updateQualitySummary() {
    if (!quality_badge_label_ || !quality_settings_label_)
        return;

    switch (video_settings_.quality) {
    case recorder_core::NvencQualityPreset::High:
        quality_badge_label_->setText(QStringLiteral("Sharper · larger files"));
        break;
    case recorder_core::NvencQualityPreset::Balanced:
        quality_badge_label_->setText(QStringLiteral("General purpose"));
        break;
    case recorder_core::NvencQualityPreset::Small:
        quality_badge_label_->setText(QStringLiteral("Smaller files"));
        break;
    }

    const QString cq = [](recorder_core::NvencQualityPreset p) -> QString {
        switch (p) {
        case recorder_core::NvencQualityPreset::High:
            return QStringLiteral("CQ 19");
        case recorder_core::NvencQualityPreset::Balanced:
            return QStringLiteral("CQ 24");
        case recorder_core::NvencQualityPreset::Small:
            return QStringLiteral("CQ 30");
        }
        return QStringLiteral("CQ 24");
    }(video_settings_.quality);

    const QString cfr_text = video_settings_.cfr ? QStringLiteral("CFR 60 fps") : QStringLiteral("VFR");
    const QString cursor_text =
        video_settings_.capture_cursor ? QStringLiteral("Cursor on") : QStringLiteral("Cursor off");
    quality_settings_label_->setText(cq + QStringLiteral(" · ") + cfr_text + QStringLiteral(" · ") + cursor_text);

    updateQualitySegmentSelection();
}

void ConfigPage::updateQualitySegmentSelection() {
    if (!quality_segment_group_)
        return;

    const auto sync_segment = [this](QPushButton* segment, recorder_core::NvencQualityPreset preset) {
        if (!segment)
            return;

        const bool selected = video_settings_.quality == preset;
        segment->setChecked(selected);
        segment->setProperty("qualitySegmentSelected", selected);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    };

    const QSignalBlocker blocker(quality_segment_group_);
    sync_segment(quality_segment_small_, recorder_core::NvencQualityPreset::Small);
    sync_segment(quality_segment_balanced_, recorder_core::NvencQualityPreset::Balanced);
    sync_segment(quality_segment_high_, recorder_core::NvencQualityPreset::High);
}

void ConfigPage::setOutputFolder(const std::filesystem::path& folder) {
    format_settings_.output_folder = folder;
    if (destination_edit_) {
        const QSignalBlocker blocker(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(folder.wstring()));
    }
    updateOutputValidationState();
    updateExampleFilename();
}

void ConfigPage::setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                                   bool active_profile_modified) {
    // Forward to the new preset API with legacy-compatible defaults.
    setPresetOptions(options, active_profile_id, QString(), active_profile_modified);
}

void ConfigPage::setActiveProfileName(const QString& profile_name) {
    active_profile_name_ = profile_name;
    updateExampleFilename();
}

void ConfigPage::setPresetOptions(const std::vector<ProfileOption>& options, const QString& selected_id,
                                  const QString& default_id, bool dirty) {
    profile_options_ = options;
    active_preset_id_ = selected_id;
    default_preset_id_ = default_id;
    preset_dirty_ = dirty;

    const QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    int active_index = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& opt = options[i];
        // All non-selected default entries get the ★ suffix so users can identify
        // the startup default while browsing the list.
        QString label = opt.label;
        if (!default_id.isEmpty() && opt.id == default_id && opt.id != selected_id)
            label += QStringLiteral(" ★");
        profile_combo_->addItem(label, opt.id);
        if (opt.id == selected_id) {
            active_index = static_cast<int>(i);
            active_preset_is_built_in_ = opt.built_in;
            active_preset_is_available_ = opt.available;
        }
    }
    if (active_index >= 0)
        profile_combo_->setCurrentIndex(active_index);

    updatePresetActionState();
}

void ConfigPage::setPresetDirty(bool dirty) {
    if (preset_dirty_ == dirty)
        return;
    preset_dirty_ = dirty;
    updatePresetActionState();
}

void ConfigPage::updatePresetActionState() {
    const bool is_default = !default_preset_id_.isEmpty() && (active_preset_id_ == default_preset_id_);
    const bool has_preset = !active_preset_id_.isEmpty();
    const bool locked = controls_locked_;

    // Dirty indicator: amber "● Unsaved" label shown only when dirty.
    if (preset_dirty_indicator_) {
        preset_dirty_indicator_->setVisible(preset_dirty_);
    }

    // Default badge: shown when the selected preset IS the startup default.
    if (preset_default_badge_) {
        preset_default_badge_->setVisible(is_default);
    }

    // Status badge (built-in / unavailable): separate from the default badge.
    if (profile_status_label_) {
        QString badge;
        if (!active_preset_is_available_) {
            badge = QStringLiteral("Unavailable");
            profile_status_label_->setProperty("stateRole", "blocked");
        } else if (active_preset_is_built_in_) {
            badge = QStringLiteral("Built-in preset");
            profile_status_label_->setProperty("stateRole", "ready");
        }
        // For user presets, suppress the badge — the default badge above is enough.
        profile_status_label_->setText(badge);
        profile_status_label_->setVisible(!badge.isEmpty());
        profile_status_label_->style()->unpolish(profile_status_label_);
        profile_status_label_->style()->polish(profile_status_label_);
    }

    // Save button: enabled only when dirty and not locked.
    if (preset_save_btn_) {
        preset_save_btn_->setVisible(preset_dirty_);
        preset_save_btn_->setEnabled(preset_dirty_ && !locked);
    }

    // Save As button: always visible, always enabled (unless locked).
    if (preset_save_as_btn_) {
        preset_save_as_btn_->setEnabled(!locked);
    }

    // Menu actions.
    if (save_preset_action_)
        save_preset_action_->setEnabled(preset_dirty_);
    if (save_preset_as_action_)
        save_preset_as_action_->setEnabled(true);
    if (new_preset_action_)
        new_preset_action_->setEnabled(true);
    if (duplicate_preset_action_)
        duplicate_preset_action_->setEnabled(has_preset);
    if (rename_preset_action_)
        rename_preset_action_->setEnabled(has_preset && !active_preset_is_built_in_);
    if (delete_preset_action_)
        delete_preset_action_->setEnabled(has_preset && !active_preset_is_built_in_);
    // "Set as default" is available only when the selected preset is NOT already the default.
    if (set_default_preset_action_)
        set_default_preset_action_->setEnabled(has_preset && !is_default);
    if (reset_changes_action_)
        reset_changes_action_->setEnabled(preset_dirty_);
    if (reset_to_defaults_action_)
        reset_to_defaults_action_->setEnabled(true);
}

void ConfigPage::onSavePreset() {
    emit savePresetRequested();
}

void ConfigPage::onSavePresetAs() {
    const QString name = QInputDialog::getText(this, QStringLiteral("Save As New Preset"),
                                               QStringLiteral("Preset name:"), QLineEdit::Normal, active_profile_name_);
    if (name.trimmed().isEmpty())
        return;
    emit savePresetAsRequested(name.trimmed());
}

void ConfigPage::onNewPreset() {
    emit newPresetRequested();
}

void ConfigPage::onDuplicatePreset() {
    emit duplicatePresetRequested();
}

void ConfigPage::onRenamePreset() {
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Preset"), QStringLiteral("New name:"),
                                               QLineEdit::Normal, active_profile_name_);
    if (name.trimmed().isEmpty())
        return;
    emit renamePresetRequested(name.trimmed());
}

void ConfigPage::onDeletePreset() {
    const auto answer =
        QMessageBox::warning(this, QStringLiteral("Delete Preset"),
                             QStringLiteral("Permanently delete this preset? This action cannot be undone."),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit deletePresetRequested();
}

void ConfigPage::onResetChanges() {
    emit resetChangesRequested();
}

void ConfigPage::onResetToDefaults() {
    const auto answer = QMessageBox::warning(this, QStringLiteral("Reset All to Factory Defaults"),
                                             QStringLiteral("Reset all presets and settings to factory defaults? "
                                                            "This action cannot be undone."),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit resetToDefaultsRequested();
}

void ConfigPage::onSetDefaultPreset() {
    emit setDefaultPresetRequested();
}

void ConfigPage::onBrowse() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Select Output Directory"), destination_edit_->text());
    if (!dir.isEmpty()) {
        destination_edit_->setText(dir);
        onDestinationEditingFinished();
    }
}

void ConfigPage::onDestinationEditingFinished() {
    const auto normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
    if (normalized.result == OutputFolderPolicyResult::Ok) {
        destination_edit_->setText(QString::fromStdWString(normalized.normalized_input));
        format_settings_.output_folder = normalized.resolved_path;
    }
    emitCurrentFormatSettings();
}

void ConfigPage::onPatternEditingFinished() {
    const auto normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
    if (normalized.result == FilenamePatternPolicyResult::Ok) {
        naming_edit_->setText(QString::fromStdWString(normalized.normalized_pattern));
        format_settings_.naming_pattern = normalized.normalized_pattern;
    }
    emitCurrentFormatSettings();
}

void ConfigPage::updateOutputValidationState() {
    if (!destination_edit_ || !naming_edit_)
        return;

    if (folder_validation_label_) {
        const auto folder_normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
        if (folder_normalized.result == OutputFolderPolicyResult::Ok) {
            folder_validation_label_->clear();
            folder_validation_label_->setVisible(false);
        } else {
            folder_validation_label_->setText(
                QString::fromStdWString(OutputFolderPolicyMessage(folder_normalized.result)));
            folder_validation_label_->setVisible(true);
        }
    }

    if (pattern_validation_label_) {
        const auto pattern_normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
        if (pattern_normalized.result == FilenamePatternPolicyResult::Ok) {
            pattern_validation_label_->clear();
            pattern_validation_label_->setVisible(false);
        } else {
            pattern_validation_label_->setText(
                QString::fromStdWString(FilenamePatternPolicyMessage(pattern_normalized.result)));
            pattern_validation_label_->setVisible(true);
        }
    }
}

void ConfigPage::updateExampleFilename() {
    if (!example_filename_label_)
        return;

    const auto output_path =
        BuildOutputPath(format_settings_.output_folder, format_settings_.naming_pattern, format_settings_.container,
                        std::time(nullptr), ExamplePreviewContext(active_profile_name_, format_settings_));
    example_filename_label_->setText(QStringLiteral("Example: ") +
                                     QString::fromStdWString(output_path.filename().wstring()));
}

void ConfigPage::setAudioUiState(const capability::AudioUiState& state) {
    audio_ui_state_ = state;
    applyAudioConfigurationState();
}

void ConfigPage::applyAudioConfigurationState() {
    const AudioConfigurationSnapshot snap =
        PresentationStateBuilder::BuildAudioConfiguration(audio_ui_state_, controls_locked_);

    const bool is_window = (snap.target_kind == capability::CaptureTargetKind::Window);

    // App section visibility (target-kind policy).
    if (app_row_section_)
        app_row_section_->setVisible(snap.app.visible);

    // System audio row labels (target-kind-specific).
    if (sys_enabled_check_) {
        sys_enabled_check_->setText(is_window ? QStringLiteral("Other system audio")
                                              : QStringLiteral("Computer audio"));
    }
    if (sys_source_label_) {
        sys_source_label_->setText(
            is_window ? QStringLiteral("Also records audio from other applications and Windows.")
                      : QStringLiteral("Records all sound played through the selected output device."));
    }

    // Apply audio row widget states atomically.
    // Required invariant: controls_enabled = visible && available && !controls_locked_
    {
        const QSignalBlocker ab(app_enabled_check_);
        const QSignalBlocker as(app_separate_check_);
        const QSignalBlocker mb(mic_enabled_check_);
        const QSignalBlocker ms(mic_separate_check_);
        const QSignalBlocker sb(sys_enabled_check_);
        const QSignalBlocker ss(sys_separate_check_);

        app_enabled_check_->setEnabled(snap.app.controls_enabled);
        app_separate_check_->setEnabled(snap.app.controls_enabled);
        app_enabled_check_->setChecked(snap.app.enabled);
        app_separate_check_->setChecked(snap.app.separate_track);

        mic_enabled_check_->setEnabled(snap.mic.controls_enabled);
        mic_separate_check_->setEnabled(snap.mic.controls_enabled);
        mic_enabled_check_->setChecked(snap.mic.enabled);
        mic_separate_check_->setChecked(snap.mic.separate_track);

        sys_enabled_check_->setEnabled(snap.system.controls_enabled);
        sys_separate_check_->setEnabled(snap.system.controls_enabled);
        sys_enabled_check_->setChecked(snap.system.enabled);
        sys_separate_check_->setChecked(snap.system.separate_track);
    }

    // Mic device combo: visible when mic source is in the plan; enabled when interactable.
    if (mic_device_combo_) {
        const QSignalBlocker mc(mic_device_combo_);
        mic_device_combo_->setVisible(snap.mic.available);
        mic_device_combo_->setEnabled(snap.mic.controls_enabled);
        if (snap.selected_mic_device_id.has_value()) {
            const auto& device_id = *snap.selected_mic_device_id;
            int idx = 0;
            for (int i = 1; i < static_cast<int>(mic_devices_.size()); ++i) {
                if (mic_devices_[static_cast<std::size_t>(i)].device_id == device_id) {
                    idx = i;
                    break;
                }
            }
            mic_device_combo_->setCurrentIndex(idx);
        } else {
            mic_device_combo_->setCurrentIndex(0);
        }
    }

    // Source description labels.
    if (app_source_label_)
        app_source_label_->setText(QStringLiteral("Records audio from the selected application."));
    if (mic_source_label_) {
        mic_source_label_->setText(snap.mic.available ? QStringLiteral("Choose the microphone used for recording.")
                                                      : QStringLiteral("Not available"));
    }

    // Summary label when no audio plan rows are present.
    const bool no_rows = audio_ui_state_.source_rows.empty();
    if (audio_summary_label_) {
        audio_summary_label_->setVisible(no_rows);
        if (no_rows)
            audio_summary_label_->setText(
                QStringLiteral("Audio sources are configured on the Record page. Open Record to set up sources."));
    }
}

void ConfigPage::emitCurrentAudioSettings() {
    emit audioSettingsChanged(audio_ui_state_);
}

void ConfigPage::setAudioMeterLevels(float sys01, float app01, float mic01, bool sys_active, bool app_active,
                                     bool mic_active) {
    auto update = [](ui::widgets::VUMeterWidget* meter, QLabel* db_label, float level01, bool active) {
        if (!meter || !db_label)
            return;
        meter->setActive(active);
        meter->setLevel(active ? level01 : 0.0f);
        if (!active) {
            db_label->setText(QStringLiteral("–"));
        } else if (level01 <= 0.0f) {
            db_label->setText(QStringLiteral("−∞"));
        } else {
            const int db_int = qRound(level01 * 60.0f - 60.0f);
            db_label->setText(QString::number(db_int) + QStringLiteral(" dB"));
        }
    };
    update(audio_sys_meter_, audio_sys_db_label_, sys01, sys_active);
    update(audio_app_meter_, audio_app_db_label_, app01, app_active);
    update(audio_mic_meter_, audio_mic_db_label_, mic01, mic_active);
}

void ConfigPage::onAudioAppToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::App)
            row.enabled = app_enabled_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioMicToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Mic)
            row.enabled = mic_enabled_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioSysToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Sys || row.kind == recorder_core::AudioSourceKind::SystemOutput)
            row.enabled = sys_enabled_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioAppSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::App)
            row.merge_with_above = !app_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioMicSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Mic)
            row.merge_with_above = !mic_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioSysSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Sys || row.kind == recorder_core::AudioSourceKind::SystemOutput)
            row.merge_with_above = !sys_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::refreshMicDevices() {
    if (!mic_device_combo_)
        return;

    const QSignalBlocker mc(mic_device_combo_);
    mic_device_combo_->clear();
    mic_devices_.clear();

    mic_device_combo_->addItem(QStringLiteral("System Default Microphone"));
    mic_devices_.push_back({});

    const auto devices = recorder_core::EnumerateAudioInputDevices();
    for (const auto& dev : devices) {
        QString label = QString::fromStdString(dev.display_name);
        if (dev.is_default)
            label += QStringLiteral(" (Default)");
        mic_device_combo_->addItem(label);
        mic_devices_.push_back(dev);
    }

    mic_device_combo_->setCurrentIndex(0);
}

void ConfigPage::onMicDeviceChanged(int index) {
    if (index <= 0 || index >= static_cast<int>(mic_devices_.size())) {
        audio_ui_state_.selected_mic_device_id = std::nullopt;
    } else {
        const auto& dev = mic_devices_[static_cast<std::size_t>(index)];
        audio_ui_state_.selected_mic_device_id =
            dev.device_id.empty() ? std::nullopt : std::optional<std::string>(dev.device_id);
    }
    emitCurrentAudioSettings();
}

void ConfigPage::setWebcamSettings(const WebcamSettings& settings) {
    webcam_settings_ = settings;
    if (webcam_setup_panel_)
        webcam_setup_panel_->applySettings(settings);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void ConfigPage::applyVisualWebcamState(bool available, bool mirror) {
    if (webcam_setup_panel_)
        webcam_setup_panel_->applyVisualState(available, mirror);
}

void ConfigPage::applyVisualPresetSaveError(bool show) {
    if (show && !visual_preset_error_label_) {
        // Lazily insert the error label directly below the preset selector row in
        // the same parent QWidget.  We locate the preset_save_btn_ parent layout
        // and insert the label after the selector row.
        QWidget* panel = preset_save_btn_ ? preset_save_btn_->parentWidget() : nullptr;
        if (!panel)
            return;
        visual_preset_error_label_ = new QLabel(panel);
        visual_preset_error_label_->setObjectName(QStringLiteral("presetVisualErrorLabel"));
        visual_preset_error_label_->setProperty("labelRole", "validationError");
        visual_preset_error_label_->setWordWrap(true);
        visual_preset_error_label_->setText(
            QStringLiteral("⚠ Name already exists. Choose a different name before saving."));
        // Insert into the panel's layout immediately after the selector row.
        if (QLayout* lay = panel->layout()) {
            // Find the position of profile_overflow_btn_ in the layout and insert
            // the error label right below the row that contains it.
            int insert_pos = lay->count(); // fallback: append
            for (int i = 0; i < lay->count(); ++i) {
                QLayoutItem* item = lay->itemAt(i);
                if (!item)
                    continue;
                if (QLayout* sub = item->layout()) {
                    for (int j = 0; j < sub->count(); ++j) {
                        QLayoutItem* sub_item = sub->itemAt(j);
                        if (sub_item && sub_item->widget() == profile_overflow_btn_) {
                            insert_pos = i + 1;
                            break;
                        }
                    }
                }
            }
            if (auto* vlay = qobject_cast<QVBoxLayout*>(lay))
                vlay->insertWidget(insert_pos, visual_preset_error_label_);
            else
                lay->addWidget(visual_preset_error_label_);
        }
    }
    if (visual_preset_error_label_)
        visual_preset_error_label_->setVisible(show);
}
#endif

void ConfigPage::setReadinessStatus(const QString& status_label) {
    if (!readiness_badge_label_)
        return;

    const QString upper = status_label.trimmed().toUpper();
    const bool blocked = upper == QStringLiteral("BLOCKED") || upper == QStringLiteral("ERROR");
    const bool ready = upper == QStringLiteral("READY");
    const bool checking = upper == QStringLiteral("CHECKING");

    readiness_badge_label_->setText(ready      ? QStringLiteral("Ready to record")
                                    : blocked  ? QStringLiteral("Recording blocked")
                                    : checking ? QStringLiteral("Checking configuration...")
                                               : QStringLiteral("Status: %1").arg(upper));

    if (readiness_detail_label_) {
        readiness_detail_label_->setText(ready ? QStringLiteral("Current configuration is compatible with this system.")
                                         : blocked  ? QStringLiteral("Open Diagnostics to review the top issue.")
                                         : checking ? QStringLiteral("Verifying system capabilities...")
                                                    : QString());
    }

    if (view_details_btn_) {
        view_details_btn_->setVisible(blocked);
    }

    // Tint the banner and colour the title to read like the prototype readiness strip
    // (green = ready, red = blocked, neutral while checking).
    const char* state = ready ? "ready" : blocked ? "blocked" : "checking";
    const char* title_state = ready ? "ready" : blocked ? "blocked" : "muted";
    const auto repolish = [](QWidget* w) {
        if (!w)
            return;
        w->style()->unpolish(w);
        w->style()->polish(w);
    };
    if (readiness_panel_) {
        readiness_panel_->setProperty("stateRole", state);
        repolish(readiness_panel_);
    }
    readiness_badge_label_->setProperty("stateRole", title_state);
    repolish(readiness_badge_label_);
}

void ConfigPage::setRecordingControlsLocked(bool locked) {
    if (controls_locked_ == locked)
        return;
    controls_locked_ = locked;

    const bool enabled = !locked;

    // Non-audio controls: locked unconditionally (no target-kind policy applies).
    profile_combo_->setEnabled(enabled);
    if (preset_save_btn_)
        preset_save_btn_->setEnabled(enabled && preset_dirty_);
    if (preset_save_as_btn_)
        preset_save_as_btn_->setEnabled(enabled);
    profile_overflow_btn_->setEnabled(enabled);
    mkv_radio_->setEnabled(enabled);
    webm_radio_->setEnabled(enabled);
    mp4_radio_->setEnabled(enabled);
    video_codec_combo_->setEnabled(enabled);
    audio_codec_combo_->setEnabled(enabled);

    quality_combo_->setEnabled(enabled);
    quality_segment_small_->setEnabled(enabled);
    quality_segment_balanced_->setEnabled(enabled);
    quality_segment_high_->setEnabled(enabled);
    cfr_check_->setEnabled(enabled);
    cursor_check_->setEnabled(enabled);

    if (webcam_setup_panel_)
        webcam_setup_panel_->setControlsLocked(locked);

    destination_edit_->setEnabled(enabled);
    browse_btn_->setEnabled(enabled);
    naming_edit_->setEnabled(enabled);

    if (lock_note_label_)
        lock_note_label_->setVisible(locked);

    // Audio source rows: use the canonical snapshot so the invariant
    //   controls_enabled = visible && available && !controls_locked_
    // holds regardless of call order between setAudioUiState and setRecordingControlsLocked.
    applyAudioConfigurationState();
}

} // namespace exosnap
