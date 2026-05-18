#include "VideoPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
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

QCheckBox* makeCheck(const QString& text, QWidget* parent) {
    auto* c = new QCheckBox(text, parent);
    c->setStyleSheet("QCheckBox { color: #C8CBD0; font-size: 13px; spacing: 6px; }"
                     "QCheckBox::indicator { width: 14px; height: 14px; border: 2px solid #3A4254;"
                     " border-radius: 2px; background: #1A2133; }"
                     "QCheckBox::indicator:checked { border: 2px solid #2468C0; background: #2468C0; }");
    return c;
}

} // namespace

VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    layout->addWidget(makeTitle("Video", content));
    layout->addWidget(makeSubLabel("Video codec, frame rate, and quality settings.", content));

    // Frame rate — fixed for MVP
    layout->addWidget(makeSectionLabel("Frame Rate Output", content));
    auto* fps_label = new QLabel("Constant 60 fps  (fixed for this release)", content);
    fps_label->setStyleSheet("color: #8A9099; font-size: 13px;");
    layout->addWidget(fps_label);

    // Resolution
    layout->addWidget(makeSectionLabel("Resolution", content));
    auto* res_src = makeRadio("Source resolution", content);
    auto* res_scale = makeRadio("Scale to…", content);
    res_src->setChecked(true);
    resolution_group_ = new QButtonGroup(this);
    resolution_group_->addButton(res_src, 0);
    resolution_group_->addButton(res_scale, 1);
    layout->addWidget(res_src);
    layout->addWidget(res_scale);

    // Codec
    layout->addWidget(makeSectionLabel("Codec", content));
    auto* av1 = makeRadio("AV1  —  Recommended", content);
    auto* hevc = makeRadio("HEVC", content);
    auto* h264 = makeRadio("H.264", content);
    av1->setChecked(true);
    codec_group_ = new QButtonGroup(this);
    codec_group_->addButton(av1, 0);
    codec_group_->addButton(hevc, 1);
    codec_group_->addButton(h264, 2);
    layout->addWidget(av1);
    layout->addWidget(hevc);
    layout->addWidget(h264);

    // Quality
    layout->addWidget(makeSectionLabel("Quality", content));
    auto* q_high = makeRadio("High quality", content);
    auto* q_balanced = makeRadio("Balanced", content);
    auto* q_small = makeRadio("Smaller files", content);
    q_balanced->setChecked(true);
    quality_group_ = new QButtonGroup(this);
    quality_group_->addButton(q_high, 0);
    quality_group_->addButton(q_balanced, 1);
    quality_group_->addButton(q_small, 2);
    layout->addWidget(q_high);
    layout->addWidget(q_balanced);
    layout->addWidget(q_small);

    // Cursor
    layout->addWidget(makeSectionLabel("Cursor", content));
    cursor_check_ = makeCheck("Capture mouse cursor", content);
    cursor_check_->setChecked(true);
    layout->addWidget(cursor_check_);

    // Advanced toggle
    expand_btn_ = new QPushButton("Advanced  ▸", content);
    expand_btn_->setStyleSheet("QPushButton { background: transparent; border: none; color: #6A8AAC;"
                               " font-size: 12px; text-align: left; padding: 6px 0; }"
                               "QPushButton:hover { color: #8AAAD0; }");
    layout->addWidget(expand_btn_);

    // Advanced panel (hidden by default)
    advanced_panel_ = new QWidget(content);
    advanced_panel_->setStyleSheet("QWidget { background: #141A26; border-radius: 6px; }");
    auto* adv_layout = new QVBoxLayout(advanced_panel_);
    adv_layout->setContentsMargins(14, 10, 14, 10);
    adv_layout->setSpacing(5);
    for (const char* txt : {
             "Effective encoder: NVENC AV1",
             "NVENC preset: P4 (balanced)",
             "Rate control: CQP",
             "CQ / CQP value: 24",
             "B-frames: 0",
             "GOP / keyframe interval: 60",
         }) {
        auto* lbl = new QLabel(txt, advanced_panel_);
        lbl->setStyleSheet("color: #8A9099; font-size: 12px;");
        adv_layout->addWidget(lbl);
    }
    advanced_panel_->hide();
    layout->addWidget(advanced_panel_);

    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(expand_btn_, &QPushButton::clicked, this, &VideoPage::onExpandAdvanced);
}

void VideoPage::onExpandAdvanced() {
    advanced_expanded_ = !advanced_expanded_;
    advanced_panel_->setVisible(advanced_expanded_);
    expand_btn_->setText(advanced_expanded_ ? "Advanced  ▾" : "Advanced  ▸");
}

} // namespace exosnap
