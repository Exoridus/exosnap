#include "ConfigPage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

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
    profile_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    profile_header->addWidget(profile_lbl);
    profile_header->addWidget(profile_combo_, 1);
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

    // ---- VIDEO SECTION (read-only) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Video"), content));
    auto* video_panel = makePanel(content);
    auto* video_panel_layout = new QVBoxLayout(video_panel);
    video_panel_layout->setContentsMargins(14, 12, 14, 12);
    video_panel_layout->setSpacing(6);
    video_summary_label_ = new QLabel(video_panel);
    video_summary_label_->setProperty("labelRole", "muted");
    video_summary_label_->setWordWrap(true);
    video_panel_layout->addWidget(video_summary_label_);
    auto* video_note = makeSubLabel(
        QStringLiteral("Video settings are configured on the Video page (CFR, quality, cursor capture)."), video_panel);
    video_note->setProperty("labelRole", "muted");
    video_panel_layout->addWidget(video_note);
    layout->addWidget(video_panel);

    // ---- AUDIO SECTION (read-only) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Audio"), content));
    auto* audio_panel = makePanel(content);
    auto* audio_panel_layout = new QVBoxLayout(audio_panel);
    audio_panel_layout->setContentsMargins(14, 12, 14, 12);
    audio_panel_layout->setSpacing(6);
    audio_summary_label_ = new QLabel(audio_panel);
    audio_summary_label_->setProperty("labelRole", "muted");
    audio_summary_label_->setWordWrap(true);
    audio_panel_layout->addWidget(audio_summary_label_);
    auto* audio_note = makeSubLabel(
        QStringLiteral("Audio sources and routing are configured on the Record page before recording."), audio_panel);
    audio_note->setProperty("labelRole", "muted");
    audio_panel_layout->addWidget(audio_note);
    layout->addWidget(audio_panel);

    // ---- OUTPUT SECTION (read-only) ----
    layout->addWidget(makeSectionLabel(QStringLiteral("Output"), content));
    auto* out_panel = makePanel(content);
    auto* out_panel_layout = new QVBoxLayout(out_panel);
    out_panel_layout->setContentsMargins(14, 12, 14, 12);
    out_panel_layout->setSpacing(6);
    output_folder_label_ = new QLabel(out_panel);
    output_folder_label_->setProperty("labelRole", "muted");
    output_folder_label_->setWordWrap(true);
    out_panel_layout->addWidget(output_folder_label_);
    auto* out_note = makeSubLabel(
        QStringLiteral("Output folder and filename pattern are configured on the Output page."), out_panel);
    out_note->setProperty("labelRole", "muted");
    out_panel_layout->addWidget(out_note);
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

    updateFormatDisplay();
}

void ConfigPage::emitCurrentFormatSettings() {
    reconcileContainerCodecRules();
    updateFormatDisplay();
    emit formatSettingsChanged(format_settings_);
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
    emit activeProfileChanged(profile_options_[static_cast<std::size_t>(index)].id);
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
}

void ConfigPage::setVideoSettings(const VideoSettingsModel& settings) {
    video_settings_ = settings;
    const QString quality_name = [&]() {
        switch (settings.quality) {
        case recorder_core::NvencQualityPreset::High:
            return QStringLiteral("High");
        case recorder_core::NvencQualityPreset::Small:
            return QStringLiteral("Small");
        default:
            return QStringLiteral("Balanced");
        }
    }();
    video_summary_label_->setText(QStringLiteral("Quality: ") + quality_name + QStringLiteral("  ·  CFR: ") +
                                  (settings.cfr ? QStringLiteral("On") : QStringLiteral("Off")) +
                                  QStringLiteral("  ·  Cursor: ") +
                                  (settings.capture_cursor ? QStringLiteral("On") : QStringLiteral("Off")));
}

void ConfigPage::setOutputFolder(const std::filesystem::path& folder) {
    output_folder_label_->setText(QString::fromStdWString(folder.wstring()));
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
        if (opt.id == active_profile_id)
            active_index = static_cast<int>(i);
    }
    if (active_index >= 0)
        profile_combo_->setCurrentIndex(active_index);
}

void ConfigPage::setActiveProfileName(const QString& profile_name) {
    active_profile_name_ = profile_name;
}

} // namespace exosnap
