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

QRadioButton* makeRadio(const QString& text, QWidget* parent) {
    auto* r = new QRadioButton(text, parent);
    r->setStyleSheet("QRadioButton { color: #C8CBD0; font-size: 13px; spacing: 6px; }"
                     "QRadioButton::indicator { width: 14px; height: 14px; }"
                     "QRadioButton::indicator:unchecked { border: 2px solid #3A4254; border-radius: 7px;"
                     " background: #1A2133; }"
                     "QRadioButton::indicator:checked { border: 2px solid #2468C0; border-radius: 7px;"
                     " background: #2468C0; }");
    return r;
}

} // namespace

OutputPage::OutputPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    layout->addWidget(makeTitle("Output", content));
    layout->addWidget(makeSubLabel("Container format, output directory, and file naming.", content));

    // Container
    layout->addWidget(makeSectionLabel("Container", content));
    auto* mkv = makeRadio("MKV", content);
    auto* mp4 = makeRadio("MP4", content);
    mkv->setChecked(true);
    container_group_ = new QButtonGroup(this);
    container_group_->addButton(mkv, 0);
    container_group_->addButton(mp4, 1);
    layout->addWidget(mkv);
    layout->addWidget(mp4);

    // MP4 info note (hidden by default)
    mp4_info_label_ = new QLabel("MP4 is less crash-resilient than MKV. If recording is interrupted unexpectedly,"
                                 " the file may require recovery or be unusable.",
                                 content);
    mp4_info_label_->setWordWrap(true);
    mp4_info_label_->setStyleSheet("background: #1A2133; border-radius: 5px; padding: 10px 14px;"
                                   " color: #8A9099; font-size: 12px;");
    mp4_info_label_->hide();
    layout->addWidget(mp4_info_label_);

    // Audio codec
    layout->addWidget(makeSectionLabel("Audio Codec", content));
    audio_codec_combo_ = new QComboBox(content);
    audio_codec_combo_->setMinimumWidth(200);
    audio_codec_combo_->setStyleSheet("QComboBox { background: #1A2133; border: 1px solid #2A3349; border-radius: 4px;"
                                      " padding: 6px 12px; color: #C8CBD0; }"
                                      "QComboBox::drop-down { border: none; }"
                                      "QComboBox QAbstractItemView { background: #1A2133; color: #C8CBD0;"
                                      " selection-background-color: #263050; border: 1px solid #2A3349; }");
    layout->addWidget(audio_codec_combo_);

    // Destination
    layout->addWidget(makeSectionLabel("Destination", content));
    auto* dest_row = new QHBoxLayout();
    dest_row->setSpacing(8);
    destination_edit_ = new QLineEdit(content);
    destination_edit_->setStyleSheet("QLineEdit { background: #1A2133; border: 1px solid #2A3349;"
                                     " border-radius: 4px; padding: 6px 10px; color: #C8CBD0; }");
    browse_btn_ = new QPushButton("Browse…", content);
    browse_btn_->setStyleSheet("QPushButton { background: #252C3C; border: 1px solid #3A4254;"
                               " border-radius: 4px; padding: 6px 14px; color: #C0C4CC; }"
                               "QPushButton:hover { background: #2E3648; }");
    dest_row->addWidget(destination_edit_);
    dest_row->addWidget(browse_btn_);
    layout->addLayout(dest_row);

    // File naming
    layout->addWidget(makeSectionLabel("File Naming", content));
    naming_edit_ = new QLineEdit(content);
    naming_edit_->setStyleSheet("QLineEdit { background: #1A2133; border: 1px solid #2A3349;"
                                " border-radius: 4px; padding: 6px 10px; color: #C8CBD0; }");
    naming_edit_->setPlaceholderText("exosnap_{date}_{time}");
    naming_edit_->setText("exosnap_{date}_{time}");
    layout->addWidget(naming_edit_);

    // Estimated output
    layout->addWidget(makeSectionLabel("Estimated Output", content));
    auto* est_label = new QLabel("Start a recording to see estimated output size.", content);
    est_label->setStyleSheet("color: #8A9099; font-size: 12px;");
    layout->addWidget(est_label);

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
