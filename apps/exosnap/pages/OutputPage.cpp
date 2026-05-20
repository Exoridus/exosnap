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
#include <QStandardPaths>
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

QRadioButton* makeRadio(const QString& text, QWidget* parent) {
    return new QRadioButton(text, parent);
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

} // namespace

OutputPage::OutputPage(QWidget* parent) : QWidget(parent) {
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

    auto* mkv = makeRadio("MKV", container_panel);
    auto* mp4 = makeRadio("MP4", container_panel);
    mkv->setChecked(true);
    container_group_ = new QButtonGroup(this);
    container_group_->addButton(mkv, 0);
    container_group_->addButton(mp4, 1);
    container_layout->addWidget(mkv);
    container_layout->addWidget(mp4);

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

    // Effective output placeholder
    layout->addWidget(makeSectionLabel("Effective Output (Placeholder)", content));
    auto* effective_panel = makePanel(content);
    auto* effective_layout = new QVBoxLayout(effective_panel);
    effective_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                         ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    effective_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);
    auto* effective_hint =
        new QLabel("Resolved output summary is shown here in a later integration pass.", effective_panel);
    effective_hint->setProperty("labelRole", "subtle");
    effective_layout->addWidget(effective_hint);
    layout->addWidget(effective_panel);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    // Default destination
    QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    destination_edit_->setText(videos + "/Exosnap");

    // Initial codec choices
    updateAudioCodecChoices();

    connect(container_group_, &QButtonGroup::idClicked, this, &OutputPage::onContainerChanged);
    connect(browse_btn_, &QPushButton::clicked, this, &OutputPage::onBrowse);
}

void OutputPage::onContainerChanged(int id) {
    updateAudioCodecChoices();
    mp4_info_label_->setVisible(id == 1);
}

void OutputPage::onBrowse() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory", destination_edit_->text());
    if (!dir.isEmpty())
        destination_edit_->setText(dir);
}

void OutputPage::updateAudioCodecChoices() {
    int container = container_group_->checkedId();
    bool is_mkv = (container == 0);

    audio_codec_combo_->blockSignals(true);
    audio_codec_combo_->clear();
    if (is_mkv) {
        audio_codec_combo_->addItem("Opus");
        audio_codec_combo_->addItem("AAC");
    } else {
        audio_codec_combo_->addItem("AAC");
    }
    audio_codec_combo_->blockSignals(false);
}

} // namespace exosnap
