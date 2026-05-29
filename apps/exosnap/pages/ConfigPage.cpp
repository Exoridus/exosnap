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
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "../../../libs/recorder_core/include/recorder_core/audio_track_model.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputPathPolicy.h"
#include "../services/WebcamService.h"

#include <ctime>

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

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
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
    layout->setContentsMargins(24, 20, 24, 32);
    layout->setSpacing(20);

    auto* header = makeSubLabel(
        QStringLiteral("Choose what to capture, which sources to include, and where the recording is saved."), content);
    header->setProperty("labelRole", "subtitle");
    layout->addWidget(header);

    auto* status_card = makePanel(content);
    status_card->setProperty("panelRole", "note");
    auto* status_layout = new QVBoxLayout(status_card);
    status_layout->setContentsMargins(14, 10, 14, 10);
    status_layout->setSpacing(4);

    readiness_badge_label_ = new QLabel(status_card);
    readiness_badge_label_->setProperty("labelRole", "section");
    status_layout->addWidget(readiness_badge_label_);

    readiness_detail_label_ = new QLabel(status_card);
    readiness_detail_label_->setProperty("labelRole", "muted");
    readiness_detail_label_->setWordWrap(true);
    status_layout->addWidget(readiness_detail_label_);

    view_details_btn_ = new QPushButton(QStringLiteral("Open Diagnostics..."), status_card);
    view_details_btn_->setProperty("role", "ghost");
    view_details_btn_->setVisible(false);
    status_layout->addWidget(view_details_btn_);

    layout->addWidget(status_card);

    lock_note_label_ = new QLabel(status_card);
    lock_note_label_->setObjectName(QStringLiteral("lockNoteLabel"));
    lock_note_label_->setProperty("labelRole", "muted");
    lock_note_label_->setWordWrap(true);
    lock_note_label_->setText(QStringLiteral("Recording settings are locked while recording."));
    lock_note_label_->setVisible(false);
    status_layout->addWidget(lock_note_label_);

    // ---- FORMAT SECTION (interactive) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Format"), content));

    auto* fmt_panel = makePanel(content);
    auto* fmt_layout = new QVBoxLayout(fmt_panel);
    fmt_layout->setContentsMargins(14, 12, 14, 12);
    fmt_layout->setSpacing(10);

    auto* profile_header = new QHBoxLayout();
    auto* profile_lbl = new QLabel(QStringLiteral("Profile"), fmt_panel);
    profile_lbl->setProperty("labelRole", "section");
    profile_combo_ = new QComboBox(fmt_panel);
    profile_combo_->setObjectName(QStringLiteral("profileCombo"));
    profile_combo_->setMinimumWidth(240);
    profile_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    profile_status_label_ = new QLabel(fmt_panel);
    profile_status_label_->setProperty("labelRole", "profileStatusBadge");
    profile_status_label_->setAlignment(Qt::AlignCenter);
    save_as_new_btn_ = new QPushButton(QStringLiteral("Save as new"), fmt_panel);
    reset_profile_btn_ = new QPushButton(QStringLiteral("Reset"), fmt_panel);
    profile_overflow_btn_ = new QToolButton(fmt_panel);
    profile_overflow_btn_->setText(QStringLiteral("Actions"));
    profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
    profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* profile_menu = new QMenu(profile_overflow_btn_);
    new_from_current_action_ = profile_menu->addAction(QStringLiteral("New from current"));
    new_from_safe_default_action_ = profile_menu->addAction(QStringLiteral("New from default"));
    profile_menu->addSeparator();
    duplicate_profile_action_ = profile_menu->addAction(QStringLiteral("Duplicate"));
    rename_profile_action_ = profile_menu->addAction(QStringLiteral("Rename"));
    delete_profile_action_ = profile_menu->addAction(QStringLiteral("Delete"));
    profile_menu->addSeparator();
    import_profiles_action_ = profile_menu->addAction(QStringLiteral("Import..."));
    export_selected_action_ = profile_menu->addAction(QStringLiteral("Export selected..."));
    export_all_users_action_ = profile_menu->addAction(QStringLiteral("Export all users..."));
    profile_menu->addSeparator();
    reset_all_action_ = profile_menu->addAction(QStringLiteral("Reset all settings + profiles"));
    profile_overflow_btn_->setMenu(profile_menu);

    profile_header->addWidget(profile_lbl);
    profile_header->addWidget(profile_combo_, 1);
    profile_header->addWidget(profile_status_label_);
    profile_header->addWidget(save_as_new_btn_);
    profile_header->addWidget(reset_profile_btn_);
    profile_header->addWidget(profile_overflow_btn_);
    fmt_layout->addLayout(profile_header);

    format_display_label_ = new QLabel(fmt_panel);
    format_display_label_->setProperty("labelRole", "muted");
    fmt_layout->addWidget(format_display_label_);

    auto* container_lbl = new QLabel(QStringLiteral("Container"), fmt_panel);
    container_lbl->setProperty("labelRole", "section");
    fmt_layout->addWidget(container_lbl);

    container_group_ = new QButtonGroup(this);
    auto* container_row = new QHBoxLayout();
    container_row->setSpacing(12);
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
    codec_row->setSpacing(12);

    auto* vcol = new QVBoxLayout();
    auto* vcodec_lbl = new QLabel(QStringLiteral("Video codec"), fmt_panel);
    vcodec_lbl->setProperty("labelRole", "section");
    video_codec_combo_ = new QComboBox(fmt_panel);
    vcol->addWidget(vcodec_lbl);
    vcol->addWidget(video_codec_combo_);
    codec_row->addLayout(vcol, 1);

    auto* acol = new QVBoxLayout();
    auto* acodec_lbl = new QLabel(QStringLiteral("Audio codec"), fmt_panel);
    acodec_lbl->setProperty("labelRole", "section");
    audio_codec_combo_ = new QComboBox(fmt_panel);
    acol->addWidget(acodec_lbl);
    acol->addWidget(audio_codec_combo_);
    codec_row->addLayout(acol, 1);

    fmt_layout->addLayout(codec_row);
    layout->addWidget(fmt_panel);

    // ---- VIDEO SECTION (interactive) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Video"), content));
    auto* video_panel = makePanel(content);
    auto* video_panel_layout = new QVBoxLayout(video_panel);
    video_panel_layout->setContentsMargins(14, 12, 14, 12);
    video_panel_layout->setSpacing(10);

    auto* qual_row = new QHBoxLayout();
    auto* qual_lbl = new QLabel(QStringLiteral("Quality preset"), video_panel);
    qual_lbl->setProperty("labelRole", "section");
    quality_combo_ = new QComboBox(video_panel);
    quality_combo_->setObjectName(QStringLiteral("videoQualityCombo"));
    quality_combo_->addItem(QStringLiteral("High"), static_cast<int>(recorder_core::NvencQualityPreset::High));
    quality_combo_->addItem(QStringLiteral("Balanced"), static_cast<int>(recorder_core::NvencQualityPreset::Balanced));
    quality_combo_->addItem(QStringLiteral("Small"), static_cast<int>(recorder_core::NvencQualityPreset::Small));
    qual_row->addWidget(qual_lbl);
    qual_row->addWidget(quality_combo_, 1);
    video_panel_layout->addLayout(qual_row);

    cfr_check_ = new QCheckBox(QStringLiteral("CFR 60 fps (constant frame rate)"), video_panel);
    cfr_check_->setChecked(video_settings_.cfr);
    video_panel_layout->addWidget(cfr_check_);

    cursor_check_ = new QCheckBox(QStringLiteral("Capture cursor"), video_panel);
    cursor_check_->setChecked(video_settings_.capture_cursor);
    video_panel_layout->addWidget(cursor_check_);
    layout->addWidget(video_panel);

    // ---- AUDIO SECTION (interactive) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Audio Sources"), content));
    auto* audio_panel = makePanel(content);
    auto* audio_panel_layout = new QVBoxLayout(audio_panel);
    audio_panel_layout->setContentsMargins(14, 12, 14, 12);
    audio_panel_layout->setSpacing(6);

    auto makeSourceRow = [&](const QString& title, QCheckBox*& enabled_check, QCheckBox*& separate_check,
                             QLabel*& source_label) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);

        enabled_check = new QCheckBox(title, audio_panel);
        separate_check = new QCheckBox(QStringLiteral("Separate track"), audio_panel);

        row->addWidget(enabled_check);
        row->addWidget(separate_check);
        row->addStretch();
        audio_panel_layout->addLayout(row);

        source_label = new QLabel(audio_panel);
        source_label->setProperty("labelRole", "muted");
        source_label->setWordWrap(true);
        source_label->setContentsMargins(20, 0, 0, 4);
        audio_panel_layout->addWidget(source_label);
    };

    makeSourceRow(QStringLiteral("Application audio"), app_enabled_check_, app_separate_check_, app_source_label_);
    makeSourceRow(QStringLiteral("Microphone"), mic_enabled_check_, mic_separate_check_, mic_source_label_);

    auto* mic_device_row = new QHBoxLayout();
    mic_device_row->setContentsMargins(20, 0, 0, 4);
    mic_device_combo_ = new QComboBox(audio_panel);
    mic_device_combo_->setObjectName(QStringLiteral("micDeviceCombo"));
    mic_device_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mic_device_combo_->setMinimumWidth(200);
    mic_device_row->addWidget(mic_device_combo_, 1);
    audio_panel_layout->addLayout(mic_device_row);

    makeSourceRow(QStringLiteral("System audio"), sys_enabled_check_, sys_separate_check_, sys_source_label_);

    audio_summary_label_ = new QLabel(audio_panel);
    audio_summary_label_->setProperty("labelRole", "muted");
    audio_summary_label_->setWordWrap(true);
    audio_summary_label_->setVisible(false);
    audio_panel_layout->addWidget(audio_summary_label_);
    layout->addWidget(audio_panel);

    // ---- WEBCAM SECTION (interactive) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Webcam"), content));
    auto* webcam_panel = makePanel(content);
    auto* webcam_panel_layout = new QVBoxLayout(webcam_panel);
    webcam_panel_layout->setContentsMargins(14, 12, 14, 12);
    webcam_panel_layout->setSpacing(4);

    webcam_enabled_check_ = new QCheckBox(QStringLiteral("Record webcam"), webcam_panel);
    webcam_panel_layout->addWidget(webcam_enabled_check_);

    auto* cam_row = new QHBoxLayout();
    cam_row->setSpacing(8);
    cam_row->setContentsMargins(20, 0, 0, 0);
    auto* cam_lbl = new QLabel(QStringLiteral("Camera:"), webcam_panel);
    webcam_device_combo_ = new QComboBox(webcam_panel);
    webcam_device_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    webcam_device_combo_->setMinimumWidth(200);
    cam_row->addWidget(cam_lbl);
    cam_row->addWidget(webcam_device_combo_, 1);
    webcam_panel_layout->addLayout(cam_row);

    webcam_info_label_ = new QLabel(webcam_panel);
    webcam_info_label_->setProperty("labelRole", "muted");
    webcam_info_label_->setWordWrap(true);
    webcam_info_label_->setContentsMargins(20, 0, 0, 4);
    webcam_panel_layout->addWidget(webcam_info_label_);

    webcam_details_btn_ = new QPushButton(QStringLiteral("Webcam details..."), webcam_panel);
    webcam_details_btn_->setObjectName(QStringLiteral("webcamDetailsBtn"));
    webcam_details_btn_->setProperty("role", "ghost");
    webcam_details_btn_->setContentsMargins(20, 0, 0, 0);
    webcam_panel_layout->addWidget(webcam_details_btn_);

    layout->addWidget(webcam_panel);

    // ---- OUTPUT SECTION (interactive) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Output"), content));
    auto* out_panel = makePanel(content);
    auto* out_panel_layout = new QVBoxLayout(out_panel);
    out_panel_layout->setContentsMargins(14, 12, 14, 12);
    out_panel_layout->setSpacing(6);

    auto* out_sub = makeSubLabel(QStringLiteral("Define output path and file naming behavior."), out_panel);
    out_panel_layout->addWidget(out_sub);

    auto* dest_row = new QHBoxLayout();
    dest_row->setSpacing(8);
    destination_edit_ = new QLineEdit(out_panel);
    destination_edit_->setObjectName(QStringLiteral("destinationEdit"));
    destination_edit_->setPlaceholderText(QString::fromStdWString(format_settings_.output_folder.wstring()));
    browse_btn_ = new QPushButton(QStringLiteral("Browse..."), out_panel);
    browse_btn_->setProperty("role", "ghost");
    dest_row->addWidget(destination_edit_, 1);
    dest_row->addWidget(browse_btn_);
    out_panel_layout->addLayout(dest_row);

    folder_validation_label_ = makeSubLabel(QString(), out_panel);
    folder_validation_label_->setProperty("labelRole", "muted");
    folder_validation_label_->setVisible(false);
    out_panel_layout->addWidget(folder_validation_label_);

    auto* naming_lbl = makeSectionLabel(QStringLiteral("Filename pattern"), out_panel);
    out_panel_layout->addWidget(naming_lbl);
    naming_edit_ = new QLineEdit(out_panel);
    naming_edit_->setObjectName(QStringLiteral("namingEdit"));
    naming_edit_->setPlaceholderText(QStringLiteral("{datetime}_{app}_{title}"));
    out_panel_layout->addWidget(naming_edit_);

    pattern_validation_label_ = makeSubLabel(QString(), out_panel);
    pattern_validation_label_->setProperty("labelRole", "muted");
    pattern_validation_label_->setVisible(false);
    out_panel_layout->addWidget(pattern_validation_label_);

    auto* tokens_help = makeSubLabel(
        QStringLiteral("Tokens: {datetime}, {date}, {time}, {timestamp}, {YYYY}, {YY}, {MM}, {DD}, {hh}, {mm}, {ss}, "
                       "{app}, {title}, {process}, {target}, {profile}, {container}, {video}, {audio}"),
        out_panel);
    tokens_help->setProperty("labelRole", "muted");
    out_panel_layout->addWidget(tokens_help);

    example_filename_label_ = makeSubLabel(QString(), out_panel);
    example_filename_label_->setProperty("labelRole", "muted");
    out_panel_layout->addWidget(example_filename_label_);
    layout->addWidget(out_panel);

    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);

    connect(container_group_, &QButtonGroup::idClicked, this, &ConfigPage::onContainerChanged);
    connect(video_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoCodecChanged);
    connect(audio_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onAudioCodecChanged);
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onProfileSelectionChanged);
    connect(quality_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConfigPage::onQualityChanged);
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
    connect(webcam_enabled_check_, &QCheckBox::toggled, this, &ConfigPage::onWebcamEnabledToggled);
    connect(webcam_device_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onWebcamDeviceChanged);
    connect(new_from_current_action_, &QAction::triggered, this, &ConfigPage::promptCreateProfileFromCurrent);
    connect(new_from_safe_default_action_, &QAction::triggered, this, &ConfigPage::promptCreateProfileFromSafeDefault);
    connect(duplicate_profile_action_, &QAction::triggered, this, &ConfigPage::duplicateActiveProfileRequested);
    connect(rename_profile_action_, &QAction::triggered, this, &ConfigPage::promptRenameActiveProfile);
    connect(delete_profile_action_, &QAction::triggered, this, &ConfigPage::onDeleteActiveProfile);
    connect(reset_profile_btn_, &QPushButton::clicked, this, &ConfigPage::resetActiveProfileRequested);
    connect(save_as_new_btn_, &QPushButton::clicked, this, &ConfigPage::promptSaveModifiedBuiltInAsNew);
    connect(import_profiles_action_, &QAction::triggered, this, &ConfigPage::onImportProfiles);
    connect(export_selected_action_, &QAction::triggered, this, &ConfigPage::onExportSelectedProfile);
    connect(export_all_users_action_, &QAction::triggered, this, &ConfigPage::onExportAllUserProfiles);
    connect(reset_all_action_, &QAction::triggered, this, &ConfigPage::onResetAllSettingsAndProfiles);
    connect(view_details_btn_, &QPushButton::clicked, this, &ConfigPage::diagnosticsRequested);
    connect(webcam_details_btn_, &QPushButton::clicked, this, &ConfigPage::webcamDetailsRequested);

    setReadinessStatus(QStringLiteral("CHECKING"));

    {
        const QSignalBlocker dd(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(format_settings_.output_folder.wstring()));
    }
    {
        const QSignalBlocker np(naming_edit_);
        naming_edit_->setText(QString::fromStdWString(format_settings_.naming_pattern));
    }
    updateAudioSourceAvailability();
    updateFormatDisplay();
    updateExampleFilename();

    QPointer<ConfigPage> safe = this;
    QTimer::singleShot(0, this, [safe]() {
        if (safe) {
            safe->refreshMicDevices();
            safe->refreshWebcamDevices();
        }
    });
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
    emitCurrentVideoSettings();
}

void ConfigPage::onCfrChanged() {
    video_settings_.cfr = cfr_check_->isChecked();
    emitCurrentVideoSettings();
}

void ConfigPage::onCursorChanged() {
    video_settings_.capture_cursor = cursor_check_->isChecked();
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
    format_display_label_->setText(summary);
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
    active_profile_is_built_in_ = opt.built_in;
    active_profile_is_modified_ = opt.modified;
    active_profile_is_available_ = opt.available;
    updateProfileActionState();
    emit activeProfileChanged(opt.id);
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
    Q_UNUSED(active_profile_modified);
    profile_options_ = options;

    const QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    int active_index = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& opt = options[i];
        QString label = opt.label;
        if (opt.modified)
            label += QStringLiteral(" *");
        if (!opt.available)
            label += QStringLiteral(" (unavailable)");
        profile_combo_->addItem(label, opt.id);
        if (opt.id == active_profile_id) {
            active_index = static_cast<int>(i);
            active_profile_is_built_in_ = opt.built_in;
            active_profile_is_modified_ = opt.modified;
            active_profile_is_available_ = opt.available;
        }
    }
    if (active_index >= 0)
        profile_combo_->setCurrentIndex(active_index);
    updateProfileActionState();
}

void ConfigPage::setActiveProfileName(const QString& profile_name) {
    active_profile_name_ = profile_name;
    updateExampleFilename();
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

    const auto findRow = [&](recorder_core::AudioSourceKind kind) -> const recorder_core::AudioSourceRow* {
        for (const auto& row : state.source_rows) {
            if (row.kind == kind)
                return &row;
        }
        if (kind == recorder_core::AudioSourceKind::Sys) {
            for (const auto& row : state.source_rows) {
                if (row.kind == recorder_core::AudioSourceKind::SystemOutput)
                    return &row;
            }
        }
        return nullptr;
    };

    const auto* app_row = findRow(recorder_core::AudioSourceKind::App);
    const auto* mic_row = findRow(recorder_core::AudioSourceKind::Mic);
    const auto* sys_row = findRow(recorder_core::AudioSourceKind::Sys);

    const QSignalBlocker ab(app_enabled_check_);
    const QSignalBlocker as(app_separate_check_);
    const QSignalBlocker mb(mic_enabled_check_);
    const QSignalBlocker ms(mic_separate_check_);
    const QSignalBlocker sb(sys_enabled_check_);
    const QSignalBlocker ss(sys_separate_check_);

    app_enabled_check_->setEnabled(app_row != nullptr);
    app_separate_check_->setEnabled(app_row != nullptr);
    app_enabled_check_->setChecked(app_row ? app_row->enabled : false);
    app_separate_check_->setChecked(app_row ? !app_row->merge_with_above : false);

    mic_enabled_check_->setEnabled(mic_row != nullptr);
    mic_separate_check_->setEnabled(mic_row != nullptr);
    mic_enabled_check_->setChecked(mic_row ? mic_row->enabled : false);
    mic_separate_check_->setChecked(mic_row ? !mic_row->merge_with_above : false);

    sys_enabled_check_->setEnabled(sys_row != nullptr);
    sys_separate_check_->setEnabled(sys_row != nullptr);
    sys_enabled_check_->setChecked(sys_row ? sys_row->enabled : false);
    sys_separate_check_->setChecked(sys_row ? !sys_row->merge_with_above : false);

    if (mic_device_combo_) {
        const QSignalBlocker mc(mic_device_combo_);
        const auto& device_id = state.selected_mic_device_id;
        if (!device_id.has_value()) {
            mic_device_combo_->setCurrentIndex(0);
        } else {
            int idx = 0;
            for (int i = 1; i < static_cast<int>(mic_devices_.size()); ++i) {
                if (mic_devices_[static_cast<std::size_t>(i)].device_id == *device_id) {
                    idx = i;
                    break;
                }
            }
            mic_device_combo_->setCurrentIndex(idx);
        }
    }

    updateAudioSourceAvailability();
}

void ConfigPage::updateAudioSourceAvailability() {
    const bool no_rows = audio_ui_state_.source_rows.empty();
    if (app_source_label_) {
        const bool available = app_enabled_check_ && app_enabled_check_->isEnabled();
        app_source_label_->setText(available ? QStringLiteral("Per-target, configured on Record page")
                                             : QStringLiteral("Not available for current capture target"));
    }
    if (mic_source_label_) {
        const bool available = mic_enabled_check_ && mic_enabled_check_->isEnabled();
        mic_source_label_->setText(available ? QStringLiteral("Choose the microphone used for recording.")
                                             : QStringLiteral("Not available"));
    }
    if (mic_device_combo_) {
        const bool available = mic_enabled_check_ && mic_enabled_check_->isEnabled();
        mic_device_combo_->setVisible(available);
        mic_device_combo_->setEnabled(available);
    }
    if (sys_source_label_) {
        const bool available = sys_enabled_check_ && sys_enabled_check_->isEnabled();
        sys_source_label_->setText(available ? QStringLiteral("All system audio except selected app")
                                             : QStringLiteral("Not available for current capture target"));
    }
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

    const QSignalBlocker wb(webcam_enabled_check_);
    webcam_enabled_check_->setChecked(settings.enabled);

    if (webcam_device_combo_) {
        const QSignalBlocker dc(webcam_device_combo_);
        const QString device_id = QString::fromStdString(settings.device_id);
        const int didx = webcam_device_combo_->findData(device_id);
        if (didx >= 0)
            webcam_device_combo_->setCurrentIndex(didx);
        else if (webcam_device_combo_->count() > 0)
            webcam_device_combo_->setCurrentIndex(0);
    }

    updateWebcamInfoLabel();
}

void ConfigPage::onWebcamEnabledToggled() {
    webcam_settings_.enabled = webcam_enabled_check_->isChecked();
    updateWebcamInfoLabel();
    emit webcamSettingsChanged(webcam_settings_);
}

void ConfigPage::emitCurrentWebcamSettings() {
    emit webcamSettingsChanged(webcam_settings_);
}

void ConfigPage::refreshWebcamDevices() {
    if (!webcam_device_combo_)
        return;

    const QSignalBlocker dc(webcam_device_combo_);
    const QString previous_id = webcam_device_combo_->currentData().toString();
    webcam_device_combo_->clear();

    const auto devices = WebcamService::EnumerateDevices();
    for (const auto& device : devices) {
        webcam_device_combo_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
    }

    if (devices.empty()) {
        webcam_device_combo_->addItem(QStringLiteral("No camera found"), QString());
        webcam_device_combo_->setEnabled(false);
    } else {
        webcam_device_combo_->setEnabled(true);
        const int idx = webcam_device_combo_->findData(previous_id);
        webcam_device_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
        if (!webcam_settings_.device_id.empty()) {
            const int sidx = webcam_device_combo_->findData(QString::fromStdString(webcam_settings_.device_id));
            if (sidx >= 0)
                webcam_device_combo_->setCurrentIndex(sidx);
        }
    }

    // Propagate auto-selected device so Setup and WebcamPage stay in sync.
    // Only applies when settings had no prior device and a real device was found.
    if (webcam_settings_.device_id.empty() && webcam_device_combo_->isEnabled()) {
        const QString auto_id = webcam_device_combo_->currentData().toString();
        if (!auto_id.isEmpty()) {
            webcam_settings_.device_id = auto_id.toStdString();
            emit webcamSettingsChanged(webcam_settings_);
        }
    }

    updateWebcamInfoLabel();
}

void ConfigPage::onWebcamDeviceChanged(int index) {
    if (index < 0)
        return;
    const QString device_id = webcam_device_combo_->itemData(index).toString();
    if (device_id.isEmpty())
        return;
    webcam_settings_.device_id = device_id.toStdString();
    updateWebcamInfoLabel();
    emit webcamSettingsChanged(webcam_settings_);
}

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
}

void ConfigPage::updateWebcamInfoLabel() {
    if (!webcam_info_label_ || !webcam_device_combo_)
        return;

    const bool enabled = webcam_settings_.enabled;
    const bool has_devices = webcam_device_combo_->isEnabled();
    const QString selected_id = webcam_device_combo_->currentData().toString();
    const QString selected_name = webcam_device_combo_->currentText();

    QString info;
    if (!enabled) {
        info = QStringLiteral("Webcam recording is disabled.");
    } else if (!has_devices) {
        info = QStringLiteral("No camera found.");
    } else if (selected_id.isEmpty()) {
        info = QStringLiteral("No camera selected.");
    } else {
        info = QStringLiteral("Selected camera: %1.").arg(selected_name);
    }
    webcam_info_label_->setText(info);
}

void ConfigPage::setRecordingControlsLocked(bool locked) {
    if (controls_locked_ == locked)
        return;
    controls_locked_ = locked;

    const bool enabled = !locked;

    profile_combo_->setEnabled(enabled);
    save_as_new_btn_->setEnabled(enabled);
    reset_profile_btn_->setEnabled(enabled);
    profile_overflow_btn_->setEnabled(enabled);
    mkv_radio_->setEnabled(enabled);
    webm_radio_->setEnabled(enabled);
    mp4_radio_->setEnabled(enabled);
    video_codec_combo_->setEnabled(enabled);
    audio_codec_combo_->setEnabled(enabled);

    quality_combo_->setEnabled(enabled);
    cfr_check_->setEnabled(enabled);
    cursor_check_->setEnabled(enabled);

    app_enabled_check_->setEnabled(enabled);
    app_separate_check_->setEnabled(enabled);
    mic_enabled_check_->setEnabled(enabled);
    mic_separate_check_->setEnabled(enabled);
    mic_device_combo_->setEnabled(enabled);
    sys_enabled_check_->setEnabled(enabled);
    sys_separate_check_->setEnabled(enabled);

    webcam_enabled_check_->setEnabled(enabled);
    webcam_device_combo_->setEnabled(enabled);
    webcam_details_btn_->setEnabled(enabled);

    destination_edit_->setEnabled(enabled);
    browse_btn_->setEnabled(enabled);
    naming_edit_->setEnabled(enabled);

    if (lock_note_label_)
        lock_note_label_->setVisible(locked);
}

void ConfigPage::updateProfileActionState() {
    const bool has_profile = profile_combo_->currentIndex() >= 0;

    const bool is_builtin = active_profile_is_built_in_ && !active_profile_is_modified_;
    const bool show_save_as = active_profile_is_built_in_ && active_profile_is_modified_;
    const bool show_reset = !active_profile_is_built_in_ || active_profile_is_modified_;

    save_as_new_btn_->setVisible(show_save_as);
    save_as_new_btn_->setEnabled(show_save_as);
    reset_profile_btn_->setVisible(show_reset);
    reset_profile_btn_->setEnabled(show_reset);

    duplicate_profile_action_->setEnabled(has_profile);
    rename_profile_action_->setVisible(!active_profile_is_built_in_);
    rename_profile_action_->setEnabled(!active_profile_is_built_in_);
    delete_profile_action_->setVisible(!active_profile_is_built_in_);
    delete_profile_action_->setEnabled(!active_profile_is_built_in_);
    export_selected_action_->setEnabled(has_profile);

    QString badge;
    if (!active_profile_is_available_) {
        badge = QStringLiteral("Unavailable");
        profile_status_label_->setProperty("stateRole", "blocked");
    } else if (active_profile_is_built_in_ && active_profile_is_modified_) {
        badge = QStringLiteral("Built-in \302\267 Modified");
        profile_status_label_->setProperty("stateRole", "recording");
    } else if (active_profile_is_built_in_) {
        badge = is_builtin ? QStringLiteral("Built-in") : QString();
        profile_status_label_->setProperty("stateRole", "ready");
    } else {
        badge = QStringLiteral("User");
        profile_status_label_->setProperty("stateRole", "ready");
    }
    profile_status_label_->setText(badge);
    profile_status_label_->setVisible(!badge.isEmpty());
    profile_status_label_->style()->unpolish(profile_status_label_);
    profile_status_label_->style()->polish(profile_status_label_);
}

void ConfigPage::onImportProfiles() {
    const QString file_path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Profiles"), QString(),
                                                           QStringLiteral("Profile files (*.json);;All files (*)"));
    if (file_path.isEmpty())
        return;
    emit importProfilesRequested(file_path);
}

void ConfigPage::onExportSelectedProfile() {
    const QString file_path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Export Selected Profile"), QStringLiteral("profile.json"),
                                     QStringLiteral("Profile files (*.json);;All files (*)"));
    if (file_path.isEmpty())
        return;
    emit exportSelectedProfileRequested(file_path);
}

void ConfigPage::onExportAllUserProfiles() {
    const QString file_path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Export All User Profiles"), QStringLiteral("profiles.json"),
                                     QStringLiteral("Profile files (*.json);;All files (*)"));
    if (file_path.isEmpty())
        return;
    emit exportAllUserProfilesRequested(file_path);
}

void ConfigPage::onDeleteActiveProfile() {
    if (active_profile_is_built_in_)
        return;

    const auto answer =
        QMessageBox::warning(this, QStringLiteral("Delete Profile"),
                             QStringLiteral("Permanently delete this user profile? This action cannot be undone."),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit deleteActiveProfileRequested();
}

void ConfigPage::onResetAllSettingsAndProfiles() {
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Reset All Settings"),
        QStringLiteral("Reset all application settings, profiles, and hotkeys to factory defaults? "
                       "This action cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit resetAllSettingsAndProfilesRequested();
}

void ConfigPage::promptCreateProfileFromCurrent() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New Profile from Current"),
                                               QStringLiteral("Profile name:"), QLineEdit::Normal, QString(), &ok);
    if (ok && !name.trimmed().isEmpty())
        emit newFromCurrentRequested(name.trimmed());
}

void ConfigPage::promptCreateProfileFromSafeDefault() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New Profile from Default"),
                                               QStringLiteral("Profile name:"), QLineEdit::Normal, QString(), &ok);
    if (ok && !name.trimmed().isEmpty())
        emit newFromSafeDefaultRequested(name.trimmed());
}

void ConfigPage::promptRenameActiveProfile() {
    if (active_profile_is_built_in_)
        return;
    const int idx = profile_combo_->currentIndex();
    const QString current_name =
        (idx >= 0) ? profile_combo_->currentText().section(QStringLiteral(" ("), 0, 0) : QString();
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Rename Profile"), QStringLiteral("New profile name:"),
                              QLineEdit::Normal, current_name, &ok);
    if (ok && !name.trimmed().isEmpty())
        emit renameActiveProfileRequested(name.trimmed());
}

void ConfigPage::promptSaveModifiedBuiltInAsNew() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Save Modified Built-in as New"),
                              QStringLiteral("Save as new profile:"), QLineEdit::Normal, QString(), &ok);
    if (ok && !name.trimmed().isEmpty())
        emit saveModifiedBuiltInAsNewRequested(name.trimmed());
}

} // namespace exosnap
