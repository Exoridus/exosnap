#include "OutputPage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "../models/FilenameBuilder.h"
#include "../ui/theme/ExoSnapMetrics.h"
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

} // namespace

OutputPage::OutputPage(const OutputSettingsModel& initial_settings, QWidget* parent)
    : QWidget(parent), settings_(initial_settings) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

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

    destination_layout->addWidget(makeSectionLabel("File Naming", destination_panel));
    naming_edit_ = new QLineEdit(destination_panel);
    naming_edit_->setPlaceholderText("exosnap_{date}_{time}");
    naming_edit_->setText("exosnap_{date}_{time}");
    destination_layout->addWidget(naming_edit_);
    layout->addWidget(destination_panel);

    // Container & compatibility
    layout->addWidget(makeSectionLabel("Container & Compatibility", content));
    auto* container_panel = makePanel(content);
    auto* container_layout = new QVBoxLayout(container_panel);
    container_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                         ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    container_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    mkv_radio_ = makeRadio("MKV", container_panel);
    mp4_radio_ = makeRadio("MP4", container_panel);
    mkv_radio_->setChecked(true);
    mp4_radio_->setEnabled(false);
    mp4_radio_->setToolTip("Available in a future release");
    container_group_ = new QButtonGroup(this);
    container_group_->addButton(mkv_radio_, 0);
    container_group_->addButton(mp4_radio_, 1);
    container_layout->addWidget(mkv_radio_);
    container_layout->addWidget(mp4_radio_);

    // MP4 info note (hidden by default)
    mp4_info_label_ = new QLabel("MP4 is less crash-resilient than MKV. If recording is interrupted unexpectedly,"
                                 " the file may require recovery or be unusable.",
                                 container_panel);
    mp4_info_label_->setWordWrap(true);
    mp4_info_label_->setProperty("panelRole", "note");
    mp4_info_label_->hide();
    container_layout->addWidget(mp4_info_label_);

    container_layout->addWidget(makeSectionLabel("Audio Codec", container_panel));
    audio_codec_combo_ = new QComboBox(container_panel);
    audio_codec_combo_->setMinimumWidth(200);
    container_layout->addWidget(audio_codec_combo_);
    layout->addWidget(container_panel);

    // Recording output behavior
    layout->addWidget(makeSectionLabel("Recording Output Behavior", content));
    auto* behavior_panel = makePanel(content);
    auto* behavior_layout = new QVBoxLayout(behavior_panel);
    behavior_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    behavior_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);
    auto* est_label = new QLabel("Estimated size appears once a session is active.", behavior_panel);
    est_label->setProperty("labelRole", "muted");
    behavior_layout->addWidget(est_label);
    behavior_layout->addWidget(
        makeSubLabel("Container and codec selections are applied when recording starts.", behavior_panel));
    layout->addWidget(behavior_panel);

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

    applySettingsToUi();
    updateEffectiveOutputPreview();

    connect(container_group_, &QButtonGroup::idClicked, this, &OutputPage::onContainerChanged);
    connect(browse_btn_, &QPushButton::clicked, this, &OutputPage::onBrowse);
    connect(destination_edit_, &QLineEdit::textChanged, this, [this](const QString&) { emitCurrentSettings(); });
    connect(destination_edit_, &QLineEdit::editingFinished, this, &OutputPage::emitCurrentSettings);
    connect(naming_edit_, &QLineEdit::textChanged, this, [this](const QString&) { emitCurrentSettings(); });
    connect(audio_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { emitCurrentSettings(); });
}

void OutputPage::onContainerChanged(int id) {
    Q_UNUSED(id);
    if (mkv_radio_ != nullptr) {
        mkv_radio_->setChecked(true);
    }
    settings_.container = capability::Container::Matroska;
    updateAudioCodecChoices();
    mp4_info_label_->setVisible(false);
    emitCurrentSettings();
}

void OutputPage::onBrowse() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", destination_edit_->text());
    if (!dir.isEmpty()) {
        destination_edit_->setText(dir);
    }
}

void OutputPage::applySettingsToUi() {
    QSignalBlocker block_destination(destination_edit_);
    QSignalBlocker block_naming(naming_edit_);
    QSignalBlocker block_container(container_group_);
    QSignalBlocker block_codec(audio_codec_combo_);

    if (settings_.container != capability::Container::Matroska) {
        settings_.container = capability::Container::Matroska;
    }

    destination_edit_->setText(QString::fromStdWString(settings_.output_folder.wstring()));
    naming_edit_->setText(QString::fromStdWString(settings_.naming_pattern));
    mkv_radio_->setChecked(true);
    mp4_info_label_->setVisible(false);

    updateAudioCodecChoices();
    const int codec_index = settings_.audio_codec == capability::AudioCodec::AacMf
                                ? audio_codec_combo_->findText("AAC")
                                : audio_codec_combo_->findText("Opus");
    audio_codec_combo_->setCurrentIndex(codec_index >= 0 ? codec_index : 0);
}

void OutputPage::emitCurrentSettings() {
    settings_.output_folder = std::filesystem::path(destination_edit_->text().toStdWString());
    settings_.naming_pattern = naming_edit_->text().toStdWString();
    settings_.container = capability::Container::Matroska; // MP4 disabled in MVP

    const QString codec_text = audio_codec_combo_->currentText().toLower();
    if (codec_text.contains("aac")) {
        settings_.audio_codec = capability::AudioCodec::AacMf;
    } else {
        settings_.audio_codec = capability::AudioCodec::Opus;
    }

    updateEffectiveOutputPreview();
    emit outputSettingsChanged(settings_);
}

void OutputPage::updateAudioCodecChoices() {
    const QString previous_codec = audio_codec_combo_->currentText().toLower();
    const bool is_mkv = container_group_->checkedId() != 1;
    audio_codec_combo_->clear();
    if (is_mkv) {
        audio_codec_combo_->addItem("Opus");
        audio_codec_combo_->addItem("AAC");
    } else {
        audio_codec_combo_->addItem("AAC");
    }

    int desired_index =
        previous_codec.contains("aac") ? audio_codec_combo_->findText("AAC") : audio_codec_combo_->findText("Opus");
    if (desired_index < 0) {
        desired_index = 0;
    }
    audio_codec_combo_->setCurrentIndex(desired_index);
}

void OutputPage::updateEffectiveOutputPreview() {
    if (effective_output_path_label_ == nullptr) {
        return;
    }

    const auto output_path =
        BuildOutputPath(settings_.output_folder, settings_.naming_pattern, settings_.container, std::time(nullptr));
    effective_output_path_label_->setText(QString::fromStdWString(output_path.wstring()));
}

} // namespace exosnap
