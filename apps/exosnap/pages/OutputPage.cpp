#include "OutputPage.h"

#include <QAction>
#include <QButtonGroup>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "../models/FilenameBuilder.h"
#include "../models/OutputPathPolicy.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
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

QRadioButton* makeRadio(const QString& text, QWidget* parent) {
    return new QRadioButton(text, parent);
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

int VideoCodecToInt(capability::VideoCodec codec) {
    return static_cast<int>(codec);
}

int AudioCodecToInt(capability::AudioCodec codec) {
    return static_cast<int>(codec);
}

capability::VideoCodec IntToVideoCodec(int value) {
    if (value == static_cast<int>(capability::VideoCodec::Av1Nvenc)) {
        return capability::VideoCodec::Av1Nvenc;
    }
    if (value == static_cast<int>(capability::VideoCodec::HevcNvenc)) {
        return capability::VideoCodec::HevcNvenc;
    }
    return capability::VideoCodec::H264Nvenc;
}

capability::AudioCodec IntToAudioCodec(int value) {
    if (value == static_cast<int>(capability::AudioCodec::Opus)) {
        return capability::AudioCodec::Opus;
    }
    if (value == static_cast<int>(capability::AudioCodec::Pcm)) {
        return capability::AudioCodec::Pcm;
    }
    return capability::AudioCodec::AacMf;
}

} // namespace

OutputPage::OutputPage(const OutputSettingsModel& initial_settings, QWidget* parent)
    : QWidget(parent), settings_(initial_settings), last_valid_output_folder_(initial_settings.output_folder),
      last_valid_naming_pattern_(initial_settings.naming_pattern) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // Profiles
    layout->addWidget(makeSectionLabel("Recording Profile", content));
    auto* profile_panel = makePanel(content);
    auto* profile_layout = new QVBoxLayout(profile_panel);
    profile_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                       ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    profile_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    auto* profile_header_row = new QHBoxLayout();
    profile_header_row->setContentsMargins(0, 0, 0, 0);
    profile_header_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    profile_combo_ = new QComboBox(profile_panel);
    profile_combo_->setMinimumWidth(300);
    profile_combo_->setMaximumWidth(540);
    profile_combo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    profile_status_label_ = new QLabel("Built-in", profile_panel);
    profile_status_label_->setProperty("labelRole", "profileStatusBadge");
    profile_status_label_->setAlignment(Qt::AlignCenter);
    save_as_new_btn_ = new QPushButton("Save as new profile", profile_panel);
    reset_profile_btn_ = new QPushButton("Reset", profile_panel);
    profile_overflow_btn_ = new QToolButton(profile_panel);
    profile_overflow_btn_->setText("Actions");
    profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
    profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    auto* profile_menu = new QMenu(profile_overflow_btn_);
    new_from_current_action_ = profile_menu->addAction("New from current");
    new_from_safe_default_action_ = profile_menu->addAction("New from default");
    profile_menu->addSeparator();
    duplicate_profile_action_ = profile_menu->addAction("Duplicate");
    rename_profile_action_ = profile_menu->addAction("Rename");
    delete_profile_action_ = profile_menu->addAction("Delete");
    profile_menu->addSeparator();
    import_profiles_action_ = profile_menu->addAction("Import…");
    export_selected_action_ = profile_menu->addAction("Export selected…");
    export_all_users_action_ = profile_menu->addAction("Export all users…");
    profile_menu->addSeparator();
    reset_all_action_ = profile_menu->addAction("Reset all settings + profiles");
    profile_overflow_btn_->setMenu(profile_menu);

    profile_header_row->addWidget(profile_combo_);
    profile_header_row->addWidget(profile_status_label_);
    profile_header_row->addStretch(1);
    profile_header_row->addWidget(save_as_new_btn_);
    profile_header_row->addWidget(reset_profile_btn_);
    profile_header_row->addWidget(profile_overflow_btn_);
    profile_layout->addLayout(profile_header_row);

    auto* profile_note = makeSubLabel("Profile actions adapt to the selected profile type.", profile_panel);
    profile_note->setProperty("labelRole", "muted");
    profile_layout->addWidget(profile_note);
    layout->addWidget(profile_panel);

    // Destination
    layout->addWidget(makeSectionLabel("Destination", content));
    auto* destination_panel = makePanel(content);
    auto* destination_layout = new QVBoxLayout(destination_panel);
    destination_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                           ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    destination_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    destination_layout->addWidget(makeSubLabel("Define output path and file naming behavior.", destination_panel));

    auto* dest_row = new QHBoxLayout();
    dest_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    destination_edit_ = new QLineEdit(destination_panel);
    browse_btn_ = new QPushButton("Browse…", destination_panel);
    browse_btn_->setProperty("role", "ghost");
    dest_row->addWidget(destination_edit_);
    dest_row->addWidget(browse_btn_);
    destination_layout->addLayout(dest_row);

    folder_validation_label_ = makeSubLabel("", destination_panel);
    folder_validation_label_->setProperty("labelRole", "muted");
    folder_validation_label_->setVisible(false);
    destination_layout->addWidget(folder_validation_label_);

    destination_layout->addWidget(makeSectionLabel("Filename Pattern", destination_panel));
    naming_edit_ = new QLineEdit(destination_panel);
    naming_edit_->setPlaceholderText("{datetime}_{app}_{title}");
    naming_edit_->setText("{datetime}_{app}_{title}");
    destination_layout->addWidget(naming_edit_);

    pattern_validation_label_ = makeSubLabel("", destination_panel);
    pattern_validation_label_->setProperty("labelRole", "muted");
    pattern_validation_label_->setVisible(false);
    destination_layout->addWidget(pattern_validation_label_);

    auto* tokens_help =
        makeSubLabel("Tokens: {datetime}, {date}, {time}, {timestamp}, {YYYY}, {YY}, {MM}, {DD}, {hh}, {mm}, {ss}, "
                     "{app}, {title}, {process}, {target}, {profile}, {container}, {video}, {audio}",
                     destination_panel);
    tokens_help->setProperty("labelRole", "muted");
    destination_layout->addWidget(tokens_help);
    auto* tokens_example = makeSubLabel("Example: {profile}/{app}/{datetime}", destination_panel);
    tokens_example->setProperty("labelRole", "muted");
    destination_layout->addWidget(tokens_example);
    layout->addWidget(destination_panel);

    // Container & compatibility
    layout->addWidget(makeSectionLabel("Container & Compatibility", content));
    auto* container_panel = makePanel(content);
    auto* container_layout = new QVBoxLayout(container_panel);
    container_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                         ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    container_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    mkv_radio_ = makeRadio("MKV (Matroska)", container_panel);
    webm_radio_ = makeRadio("WebM", container_panel);
    mp4_radio_ = makeRadio("MP4", container_panel);
    mkv_radio_->setChecked(true);
    container_group_ = new QButtonGroup(this);
    container_group_->addButton(mkv_radio_, 0);
    container_group_->addButton(webm_radio_, 1);
    container_group_->addButton(mp4_radio_, 2);
    container_layout->addWidget(mkv_radio_);
    container_layout->addWidget(webm_radio_);
    container_layout->addWidget(mp4_radio_);

    mp4_info_label_ = new QLabel("MP4 is less crash-resilient than MKV or WebM. If recording is interrupted "
                                 "unexpectedly, the file may require recovery or be unusable.",
                                 container_panel);
    mp4_info_label_->setWordWrap(true);
    mp4_info_label_->setProperty("panelRole", "note");
    mp4_info_label_->hide();
    container_layout->addWidget(mp4_info_label_);

    container_layout->addWidget(makeSectionLabel("Video Codec", container_panel));
    video_codec_combo_ = new QComboBox(container_panel);
    video_codec_combo_->setMinimumWidth(200);
    video_codec_combo_->setMaximumWidth(320);
    video_codec_combo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    container_layout->addWidget(video_codec_combo_);

    container_layout->addWidget(makeSectionLabel("Audio Codec", container_panel));
    audio_codec_combo_ = new QComboBox(container_panel);
    audio_codec_combo_->setMinimumWidth(200);
    audio_codec_combo_->setMaximumWidth(320);
    audio_codec_combo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    container_layout->addWidget(audio_codec_combo_);
    layout->addWidget(container_panel);

    // Effective output preview
    layout->addWidget(makeSectionLabel("Effective Output", content));
    auto* effective_panel = makePanel(content);
    auto* effective_layout = new QVBoxLayout(effective_panel);
    effective_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                         ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    effective_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);
    auto* effective_title = new QLabel("Next recording will be saved as:", effective_panel);
    effective_title->setProperty("labelRole", "subtle");
    effective_layout->addWidget(effective_title);
    effective_output_path_label_ = new QLabel(effective_panel);
    effective_output_path_label_->setProperty("labelRole", "destinationPath");
    effective_output_path_label_->setTextFormat(Qt::PlainText);
    effective_layout->addWidget(effective_output_path_label_);
    layout->addWidget(effective_panel);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(profile_combo_);
    combo_wheel_filter->installOn(video_codec_combo_);
    combo_wheel_filter->installOn(audio_codec_combo_);

    applySettingsToUi();
    updateProfileActionState();
    updateValidationState();
    updateEffectiveOutputPreview();

    connect(container_group_, &QButtonGroup::idClicked, this, &OutputPage::onContainerChanged);
    connect(video_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &OutputPage::onVideoCodecChanged);
    connect(audio_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &OutputPage::onAudioCodecChanged);
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &OutputPage::onProfileSelectionChanged);
    connect(browse_btn_, &QPushButton::clicked, this, &OutputPage::onBrowse);
    connect(destination_edit_, &QLineEdit::textChanged, this, [this](const QString&) { updateValidationState(); });
    connect(destination_edit_, &QLineEdit::editingFinished, this, &OutputPage::onDestinationEditingFinished);
    connect(naming_edit_, &QLineEdit::textChanged, this, [this](const QString&) { updateValidationState(); });
    connect(naming_edit_, &QLineEdit::editingFinished, this, &OutputPage::onPatternEditingFinished);

    connect(new_from_current_action_, &QAction::triggered, this, &OutputPage::promptCreateProfileFromCurrent);
    connect(new_from_safe_default_action_, &QAction::triggered, this, &OutputPage::promptCreateProfileFromSafeDefault);
    connect(duplicate_profile_action_, &QAction::triggered, this, &OutputPage::duplicateActiveProfileRequested);
    connect(rename_profile_action_, &QAction::triggered, this, &OutputPage::promptRenameActiveProfile);
    connect(delete_profile_action_, &QAction::triggered, this, &OutputPage::onDeleteActiveProfile);
    connect(reset_profile_btn_, &QPushButton::clicked, this, &OutputPage::resetActiveProfileRequested);
    connect(save_as_new_btn_, &QPushButton::clicked, this, &OutputPage::promptSaveModifiedBuiltInAsNew);
    connect(import_profiles_action_, &QAction::triggered, this, &OutputPage::onImportProfiles);
    connect(export_selected_action_, &QAction::triggered, this, &OutputPage::onExportSelectedProfile);
    connect(export_all_users_action_, &QAction::triggered, this, &OutputPage::onExportAllUserProfiles);
    connect(reset_all_action_, &QAction::triggered, this, &OutputPage::onResetAllSettingsAndProfiles);
}

void OutputPage::setOutputSettings(const OutputSettingsModel& settings) {
    settings_ = settings;
    last_valid_output_folder_ = settings.output_folder;
    last_valid_naming_pattern_ = settings.naming_pattern;
    applySettingsToUi();
    updateValidationState();
    updateEffectiveOutputPreview();
}

void OutputPage::setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                                   bool active_profile_modified) {
    profile_options_ = options;
    active_profile_is_modified_ = active_profile_modified;

    QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    for (const auto& option : profile_options_) {
        profile_combo_->addItem(option.label, option.id);
    }

    const int index = profile_combo_->findData(active_profile_id);
    if (index >= 0) {
        profile_combo_->setCurrentIndex(index);
    }
    onProfileSelectionChanged(profile_combo_->currentIndex());
}

void OutputPage::setActiveProfileName(const QString& profile_name) {
    active_profile_name_ = profile_name;
    updateEffectiveOutputPreview();
}

QString OutputPage::activeProfileId() const {
    return profile_combo_ ? profile_combo_->currentData().toString() : QString();
}

void OutputPage::onContainerChanged(int id) {
    if (id == 2) {
        settings_.container = capability::Container::Mp4;
        mp4_info_label_->setVisible(true);
    } else if (id == 1) {
        settings_.container = capability::Container::WebM;
        mp4_info_label_->setVisible(false);
    } else {
        settings_.container = capability::Container::Matroska;
        mp4_info_label_->setVisible(false);
    }
    reconcileContainerCodecRules();
    emitCurrentSettings();
}

void OutputPage::onVideoCodecChanged(int index) {
    Q_UNUSED(index);
    if (video_codec_combo_->currentIndex() >= 0) {
        settings_.video_codec = IntToVideoCodec(video_codec_combo_->currentData().toInt());
    }
    reconcileContainerCodecRules();
    emitCurrentSettings();
}

void OutputPage::onAudioCodecChanged(int index) {
    Q_UNUSED(index);
    if (audio_codec_combo_->currentIndex() >= 0) {
        settings_.audio_codec = IntToAudioCodec(audio_codec_combo_->currentData().toInt());
    }
    reconcileContainerCodecRules();
    emitCurrentSettings();
}

void OutputPage::onBrowse() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", destination_edit_->text());
    if (!dir.isEmpty()) {
        destination_edit_->setText(dir);
        onDestinationEditingFinished();
    }
}

void OutputPage::onDestinationEditingFinished() {
    const OutputPasteSplitDecision split = AnalyzeOutputPasteInput(destination_edit_->text().toStdWString());
    if (split.kind == OutputPasteSplitKind::AutoSplitTokenPath) {
        destination_edit_->setText(QString::fromStdWString(split.folder_input));
        naming_edit_->setText(QString::fromStdWString(split.pattern_input));
    } else if (split.kind == OutputPasteSplitKind::OfferSplitFullFilePath) {
        const int choice = QMessageBox::question(
            this, QStringLiteral("Split Output File Path"),
            QStringLiteral("This looks like a full output file path. Split into folder and filename pattern?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice == QMessageBox::Yes) {
            destination_edit_->setText(QString::fromStdWString(split.folder_input));
            naming_edit_->setText(QString::fromStdWString(split.pattern_input));
        }
    }

    const NormalizedOutputFolder normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
    if (normalized.result == OutputFolderPolicyResult::Ok) {
        destination_edit_->setText(QString::fromStdWString(normalized.normalized_input));
        settings_.output_folder = normalized.resolved_path;
        last_valid_output_folder_ = normalized.resolved_path;
    }

    updateValidationState();
    emitCurrentSettings();
}

void OutputPage::onPatternEditingFinished() {
    const NormalizedFilenamePattern normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
    if (normalized.result == FilenamePatternPolicyResult::Ok) {
        naming_edit_->setText(QString::fromStdWString(normalized.normalized_pattern));
        settings_.naming_pattern = normalized.normalized_pattern;
        last_valid_naming_pattern_ = normalized.normalized_pattern;
    }
    updateValidationState();
    emitCurrentSettings();
}

void OutputPage::onProfileSelectionChanged(int index) {
    if (index < 0 || index >= static_cast<int>(profile_options_.size())) {
        return;
    }

    const ProfileOption& selected = profile_options_[static_cast<std::size_t>(index)];
    active_profile_is_built_in_ = selected.built_in;
    active_profile_is_modified_ = selected.modified;
    active_profile_is_available_ = selected.available;
    active_profile_name_ = selected.label;

    QString status;
    if (!selected.available) {
        status = QStringLiteral("Unavailable");
    } else if (selected.built_in && selected.modified) {
        status = QStringLiteral("Built-in · Modified");
    } else if (selected.built_in) {
        status = QStringLiteral("Built-in");
    } else {
        status = QStringLiteral("User");
    }
    profile_status_label_->setText(status);
    profile_status_label_->setToolTip(selected.availability_reason.trimmed());
    updateProfileActionState();
    updateEffectiveOutputPreview();
    emit activeProfileChanged(selected.id);
}

void OutputPage::applySettingsToUi() {
    QSignalBlocker block_destination(destination_edit_);
    QSignalBlocker block_naming(naming_edit_);
    QSignalBlocker block_container(container_group_);
    QSignalBlocker block_video(video_codec_combo_);
    QSignalBlocker block_audio(audio_codec_combo_);

    destination_edit_->setText(QString::fromStdWString(settings_.output_folder.wstring()));
    naming_edit_->setText(QString::fromStdWString(settings_.naming_pattern));

    if (settings_.container == capability::Container::Mp4) {
        mp4_radio_->setChecked(true);
        mp4_info_label_->setVisible(true);
    } else if (settings_.container == capability::Container::WebM) {
        webm_radio_->setChecked(true);
        mp4_info_label_->setVisible(false);
    } else {
        settings_.container = capability::Container::Matroska;
        mkv_radio_->setChecked(true);
        mp4_info_label_->setVisible(false);
    }

    reconcileContainerCodecRules();
    updateVideoCodecChoices();
    updateAudioCodecChoices();
}

void OutputPage::emitCurrentSettings() {
    const auto folder_normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
    if (folder_normalized.result == OutputFolderPolicyResult::Ok) {
        settings_.output_folder = folder_normalized.resolved_path;
        last_valid_output_folder_ = settings_.output_folder;
    } else {
        settings_.output_folder = last_valid_output_folder_;
    }

    const auto pattern_normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
    if (pattern_normalized.result == FilenamePatternPolicyResult::Ok) {
        settings_.naming_pattern = pattern_normalized.normalized_pattern;
        last_valid_naming_pattern_ = settings_.naming_pattern;
    } else {
        settings_.naming_pattern = last_valid_naming_pattern_;
    }

    const int cid = container_group_->checkedId();
    if (cid == 2) {
        settings_.container = capability::Container::Mp4;
    } else if (cid == 1) {
        settings_.container = capability::Container::WebM;
    } else {
        settings_.container = capability::Container::Matroska;
    }

    if (video_codec_combo_->currentIndex() >= 0) {
        settings_.video_codec = IntToVideoCodec(video_codec_combo_->currentData().toInt());
    }
    if (audio_codec_combo_->currentIndex() >= 0) {
        settings_.audio_codec = IntToAudioCodec(audio_codec_combo_->currentData().toInt());
    }

    reconcileContainerCodecRules();
    updateValidationState();
    updateEffectiveOutputPreview();
    emit outputSettingsChanged(settings_);
}

void OutputPage::reconcileContainerCodecRules() {
    if (settings_.container == capability::Container::Mp4) {
        settings_.video_codec = capability::VideoCodec::H264Nvenc;
        settings_.audio_codec = capability::AudioCodec::AacMf;
    } else if (settings_.container == capability::Container::WebM) {
        settings_.video_codec = capability::VideoCodec::Av1Nvenc;
        settings_.audio_codec = capability::AudioCodec::Opus;
    } else {
        if (settings_.video_codec == capability::VideoCodec::HevcNvenc) {
            settings_.video_codec = capability::VideoCodec::H264Nvenc;
        }
        if (settings_.video_codec == capability::VideoCodec::H264Nvenc &&
            settings_.audio_codec == capability::AudioCodec::Opus) {
            settings_.audio_codec = capability::AudioCodec::AacMf;
        }
        if (settings_.audio_codec == capability::AudioCodec::Pcm) {
            settings_.audio_codec = capability::AudioCodec::AacMf;
        }
    }

    updateVideoCodecChoices();
    updateAudioCodecChoices();
}

void OutputPage::updateVideoCodecChoices() {
    QSignalBlocker blocker(video_codec_combo_);
    const capability::VideoCodec desired = settings_.video_codec;
    video_codec_combo_->clear();

    if (settings_.container == capability::Container::Mp4) {
        video_codec_combo_->addItem(VideoCodecLabel(capability::VideoCodec::H264Nvenc),
                                    VideoCodecToInt(capability::VideoCodec::H264Nvenc));
    } else if (settings_.container == capability::Container::WebM) {
        video_codec_combo_->addItem(VideoCodecLabel(capability::VideoCodec::Av1Nvenc),
                                    VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
    } else {
        video_codec_combo_->addItem(VideoCodecLabel(capability::VideoCodec::H264Nvenc),
                                    VideoCodecToInt(capability::VideoCodec::H264Nvenc));
        video_codec_combo_->addItem(VideoCodecLabel(capability::VideoCodec::Av1Nvenc),
                                    VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
    }

    int index = video_codec_combo_->findData(VideoCodecToInt(desired));
    if (index < 0) {
        index = 0;
    }
    video_codec_combo_->setCurrentIndex(index);
    settings_.video_codec = IntToVideoCodec(video_codec_combo_->currentData().toInt());
    video_codec_combo_->setEnabled(video_codec_combo_->count() > 1);
}

void OutputPage::updateAudioCodecChoices() {
    QSignalBlocker blocker(audio_codec_combo_);
    const capability::AudioCodec desired = settings_.audio_codec;
    audio_codec_combo_->clear();

    if (settings_.container == capability::Container::Mp4) {
        audio_codec_combo_->addItem(AudioCodecLabel(capability::AudioCodec::AacMf),
                                    AudioCodecToInt(capability::AudioCodec::AacMf));
    } else if (settings_.container == capability::Container::WebM) {
        audio_codec_combo_->addItem(AudioCodecLabel(capability::AudioCodec::Opus),
                                    AudioCodecToInt(capability::AudioCodec::Opus));
    } else if (settings_.video_codec == capability::VideoCodec::H264Nvenc) {
        audio_codec_combo_->addItem(AudioCodecLabel(capability::AudioCodec::AacMf),
                                    AudioCodecToInt(capability::AudioCodec::AacMf));
    } else {
        audio_codec_combo_->addItem(AudioCodecLabel(capability::AudioCodec::AacMf),
                                    AudioCodecToInt(capability::AudioCodec::AacMf));
        audio_codec_combo_->addItem(AudioCodecLabel(capability::AudioCodec::Opus),
                                    AudioCodecToInt(capability::AudioCodec::Opus));
    }

    int index = audio_codec_combo_->findData(AudioCodecToInt(desired));
    if (index < 0) {
        index = 0;
    }
    audio_codec_combo_->setCurrentIndex(index);
    settings_.audio_codec = IntToAudioCodec(audio_codec_combo_->currentData().toInt());
    audio_codec_combo_->setEnabled(audio_codec_combo_->count() > 1);
}

void OutputPage::updateEffectiveOutputPreview() {
    if (effective_output_path_label_ == nullptr) {
        return;
    }

    const auto output_path =
        BuildOutputPath(settings_.output_folder, settings_.naming_pattern, settings_.container, std::time(nullptr),
                        ExamplePreviewContext(active_profile_name_, settings_));
    effective_output_path_label_->setText(QString::fromStdWString(output_path.wstring()));
}

void OutputPage::updateValidationState() {
    const auto folder_normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
    if (folder_normalized.result == OutputFolderPolicyResult::Ok) {
        folder_validation_label_->clear();
        folder_validation_label_->setVisible(false);
    } else {
        folder_validation_label_->setText(QString::fromStdWString(OutputFolderPolicyMessage(folder_normalized.result)));
        folder_validation_label_->setProperty("labelRole", "mutedWarning");
        folder_validation_label_->setVisible(true);
    }

    const auto pattern_normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
    if (pattern_normalized.result == FilenamePatternPolicyResult::Ok) {
        pattern_validation_label_->clear();
        pattern_validation_label_->setVisible(false);
    } else {
        pattern_validation_label_->setText(
            QString::fromStdWString(FilenamePatternPolicyMessage(pattern_normalized.result)));
        pattern_validation_label_->setProperty("labelRole", "mutedWarning");
        pattern_validation_label_->setVisible(true);
    }
}

void OutputPage::updateProfileActionState() {
    const bool has_profile = profile_combo_ && profile_combo_->currentIndex() >= 0;
    const bool can_reset = has_profile && (!active_profile_is_built_in_ || active_profile_is_modified_);
    const bool can_save_as_new = has_profile && active_profile_is_built_in_ && active_profile_is_modified_;
    const bool user_profile_actions = has_profile && !active_profile_is_built_in_;

    save_as_new_btn_->setVisible(can_save_as_new);
    save_as_new_btn_->setEnabled(can_save_as_new);
    reset_profile_btn_->setVisible(can_reset);
    reset_profile_btn_->setEnabled(can_reset);

    duplicate_profile_action_->setEnabled(has_profile);
    rename_profile_action_->setEnabled(user_profile_actions);
    rename_profile_action_->setVisible(user_profile_actions);
    delete_profile_action_->setEnabled(user_profile_actions);
    delete_profile_action_->setVisible(user_profile_actions);
    new_from_current_action_->setEnabled(true);
    new_from_safe_default_action_->setEnabled(true);
    import_profiles_action_->setEnabled(true);
    export_selected_action_->setEnabled(has_profile);
    export_all_users_action_->setEnabled(true);
    reset_all_action_->setEnabled(true);

    const QString status_role = !has_profile                    ? QStringLiteral("muted")
                                : !active_profile_is_available_ ? QStringLiteral("blocked")
                                : active_profile_is_modified_   ? QStringLiteral("recording")
                                                                : QStringLiteral("ready");
    if (profile_status_label_->property("stateRole").toString() != status_role) {
        profile_status_label_->setProperty("stateRole", status_role);
        profile_status_label_->style()->unpolish(profile_status_label_);
        profile_status_label_->style()->polish(profile_status_label_);
        profile_status_label_->update();
    }
}

void OutputPage::onImportProfiles() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Profiles"), QString(),
                                                      QStringLiteral("ExoSnap Profiles (*.json);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit importProfilesRequested(path);
}

void OutputPage::onExportSelectedProfile() {
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Selected Profile"), QString(),
                                                      QStringLiteral("ExoSnap Profiles (*.json);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit exportSelectedProfileRequested(path);
}

void OutputPage::onExportAllUserProfiles() {
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export All User Profiles"), QString(),
                                                      QStringLiteral("ExoSnap Profiles (*.json);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    emit exportAllUserProfilesRequested(path);
}

void OutputPage::onDeleteActiveProfile() {
    if (active_profile_is_built_in_) {
        return;
    }

    const QString profile_name = profile_combo_ ? profile_combo_->currentText().trimmed() : QString();
    const QString prompt = profile_name.isEmpty()
                               ? QStringLiteral("Delete the selected user profile? This cannot be undone.")
                               : QStringLiteral("Delete profile \"%1\"? This cannot be undone.").arg(profile_name);
    const int choice = QMessageBox::warning(this, QStringLiteral("Delete Profile"), prompt,
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }
    emit deleteActiveProfileRequested();
}

void OutputPage::onResetAllSettingsAndProfiles() {
    const int choice = QMessageBox::warning(this, QStringLiteral("Reset All Settings + Profiles"),
                                            QStringLiteral("Reset all settings and user profiles to defaults?"),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }
    emit resetAllSettingsAndProfilesRequested();
}

void OutputPage::promptCreateProfileFromCurrent() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New Profile"), QStringLiteral("Profile name:"),
                                               QLineEdit::Normal, QStringLiteral("New Profile"), &ok);
    if (!ok) {
        return;
    }
    emit newFromCurrentRequested(name.trimmed());
}

void OutputPage::promptCreateProfileFromSafeDefault() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New Default-Based Profile"), QStringLiteral("Profile name:"),
                              QLineEdit::Normal, QStringLiteral("New Profile"), &ok);
    if (!ok) {
        return;
    }
    emit newFromSafeDefaultRequested(name.trimmed());
}

void OutputPage::promptRenameActiveProfile() {
    bool ok = false;
    const QString current_label = profile_combo_->currentText();
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Profile"), QStringLiteral("Profile name:"),
                                               QLineEdit::Normal, current_label, &ok);
    if (!ok) {
        return;
    }
    emit renameActiveProfileRequested(name.trimmed());
}

void OutputPage::promptSaveModifiedBuiltInAsNew() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Save Modified Built-in"), QStringLiteral("New profile name:"),
                              QLineEdit::Normal, QStringLiteral("Custom Profile"), &ok);
    if (!ok) {
        return;
    }
    emit saveModifiedBuiltInAsNewRequested(name.trimmed());
}

} // namespace exosnap
