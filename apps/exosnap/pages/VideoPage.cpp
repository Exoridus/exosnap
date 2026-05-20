#include "VideoPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/CodecCard.h"
#include "../ui/widgets/SectionRuleHeader.h"
#include "../ui/widgets/StatusPill.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace exosnap {
namespace {

QFrame* makePanel(QWidget* parent, const char* role = "panel") {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", role);
    return panel;
}

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", role);
    return label;
}

QFrame* makeDivider(QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setFrameShape(QFrame::HLine);
    divider->setProperty("frameRole", "sectionRuleLine");
    return divider;
}

void addKvRow(QGridLayout* layout, QWidget* parent, int row, const QString& key, const QString& value,
              const char* value_role) {
    auto* key_label = new QLabel(key, parent);
    key_label->setProperty("labelRole", "videoKvKey");

    auto* value_label = new QLabel(value, parent);
    value_label->setProperty("labelRole", value_role);
    value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(key_label, row, 0);
    layout->addWidget(value_label, row, 1);
}

} // namespace

VideoPage::VideoPage(QWidget* parent) : QWidget(parent) {
    auto* page_layout = new QHBoxLayout(this);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* left_content = new QWidget(scroll);
    auto* left_layout = new QVBoxLayout(left_content);
    left_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                                    ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    left_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    auto* frame_rate_header = new ui::widgets::SectionRuleHeader("FRAME RATE", left_content);
    frame_rate_header->setMeta("CFR · LOCKED THIS RELEASE");
    left_layout->addWidget(frame_rate_header);

    auto* frame_rate_panel = makePanel(left_content);
    auto* frame_rate_layout = new QHBoxLayout(frame_rate_panel);
    frame_rate_layout->setContentsMargins(14, 12, 14, 12);
    frame_rate_layout->setSpacing(10);

    auto* frame_rate_copy = new QWidget(frame_rate_panel);
    auto* frame_rate_copy_layout = new QVBoxLayout(frame_rate_copy);
    frame_rate_copy_layout->setContentsMargins(0, 0, 0, 0);
    frame_rate_copy_layout->setSpacing(2);

    auto* fps_row = new QWidget(frame_rate_copy);
    auto* fps_row_layout = new QHBoxLayout(fps_row);
    fps_row_layout->setContentsMargins(0, 0, 0, 0);
    fps_row_layout->setSpacing(8);
    fps_row_layout->addWidget(makeLabel("60.00", "videoFpsValue", fps_row));
    fps_row_layout->addWidget(makeLabel("fps", "videoFpsUnit", fps_row));
    fps_row_layout->addStretch(1);

    frame_rate_copy_layout->addWidget(fps_row);
    frame_rate_copy_layout->addWidget(
        makeLabel("Constant frame rate — variable will return in 0.5.", "videoFpsNote", frame_rate_copy));

    auto* constant_badge = makeLabel("CONSTANT", "videoFramePill", frame_rate_panel);
    constant_badge->setAlignment(Qt::AlignCenter);
    constant_badge->setFixedHeight(36);

    frame_rate_layout->addWidget(frame_rate_copy, 1);
    frame_rate_layout->addWidget(constant_badge, 0, Qt::AlignRight | Qt::AlignVCenter);
    left_layout->addWidget(frame_rate_panel);

    auto* resolution_header = new ui::widgets::SectionRuleHeader("RESOLUTION", left_content);
    left_layout->addWidget(resolution_header);

    auto* resolution_panel = makePanel(left_content);
    auto* resolution_layout = new QVBoxLayout(resolution_panel);
    resolution_layout->setContentsMargins(0, 0, 0, 0);
    resolution_layout->setSpacing(0);

    auto* source_row = new QWidget(resolution_panel);
    auto* source_row_layout = new QHBoxLayout(source_row);
    source_row_layout->setContentsMargins(14, 12, 14, 12);
    source_row_layout->setSpacing(10);
    auto* source_radio = new QRadioButton("Source resolution", source_row);
    auto* source_value = makeLabel("2560×1440", "videoResolutionValue", source_row);
    source_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    source_row_layout->addWidget(source_radio);
    source_row_layout->addStretch(1);
    source_row_layout->addWidget(source_value);
    resolution_layout->addWidget(source_row);

    resolution_layout->addWidget(makeDivider(resolution_panel));

    auto* scale_row = new QWidget(resolution_panel);
    auto* scale_row_layout = new QHBoxLayout(scale_row);
    scale_row_layout->setContentsMargins(14, 12, 14, 12);
    scale_row_layout->setSpacing(10);
    auto* scale_radio = new QRadioButton("Scale to...", scale_row);
    auto* scale_combo = new QComboBox(scale_row);
    scale_combo->addItem("1920 × 1080");
    scale_combo->addItem("1280 × 720");
    scale_combo->setMinimumWidth(220);
    scale_combo->setMaximumWidth(260);
    scale_row_layout->addWidget(scale_radio);
    scale_row_layout->addStretch(1);
    scale_row_layout->addWidget(scale_combo);
    resolution_layout->addWidget(scale_row);

    resolution_group_ = new QButtonGroup(this);
    resolution_group_->addButton(source_radio, 0);
    resolution_group_->addButton(scale_radio, 1);
    source_radio->setChecked(true);

    left_layout->addWidget(resolution_panel);

    auto* codec_header = new ui::widgets::SectionRuleHeader("CODEC", left_content);
    codec_header->setMeta("HARDWARE ENCODE");
    left_layout->addWidget(codec_header);

    auto* codec_row = new QWidget(left_content);
    auto* codec_layout = new QHBoxLayout(codec_row);
    codec_layout->setContentsMargins(0, 0, 0, 0);
    codec_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* av1_card = new ui::widgets::CodecCard("AV1", "● SELECTED", "Best compression", codec_row);
    auto* hevc_card = new ui::widgets::CodecCard("HEVC", "H.265", "Wide playback support", codec_row);
    auto* h264_card = new ui::widgets::CodecCard("H.264", "FALLBACK", "Maximum compatibility", codec_row);

    codec_cards_ = {av1_card, hevc_card, h264_card};
    av1_card->setSelected(true);

    codec_layout->addWidget(av1_card, 1);
    codec_layout->addWidget(hevc_card, 1);
    codec_layout->addWidget(h264_card, 1);
    left_layout->addWidget(codec_row);

    for (auto* card : codec_cards_)
        connect(card, &ui::widgets::CodecCard::clicked, this, [this, card]() { selectCodecCard(card); });

    auto* quality_header = new ui::widgets::SectionRuleHeader("QUALITY", left_content);
    left_layout->addWidget(quality_header);

    auto* quality_panel = makePanel(left_content);
    auto* quality_layout = new QVBoxLayout(quality_panel);
    quality_layout->setContentsMargins(0, 0, 0, 0);
    quality_layout->setSpacing(0);

    quality_group_ = new QButtonGroup(this);
    struct QualityRow {
        const char* title;
        const char* detail;
        int id;
    };
    const QualityRow quality_rows[] = {
        {"High quality", "CQ 19 · ~62 Mb/s · large files", 0},
        {"Balanced", "CQ 24 · ~38 Mb/s · default", 1},
        {"Smaller files", "CQ 30 · ~18 Mb/s · streaming-friendly", 2},
    };

    for (int i = 0; i < 3; ++i) {
        if (i != 0)
            quality_layout->addWidget(makeDivider(quality_panel));

        auto* row = new QWidget(quality_panel);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(14, 12, 14, 12);
        row_layout->setSpacing(10);

        auto* radio = new QRadioButton(quality_rows[i].title, row);
        auto* detail = makeLabel(quality_rows[i].detail, "videoQualityDetail", row);
        detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        quality_group_->addButton(radio, quality_rows[i].id);
        row_layout->addWidget(radio);
        row_layout->addStretch(1);
        row_layout->addWidget(detail);
        quality_layout->addWidget(row);
    }
    if (auto* balanced = quality_group_->button(1))
        balanced->setChecked(true);

    left_layout->addWidget(quality_panel);

    auto* cursor_header = new ui::widgets::SectionRuleHeader("CURSOR", left_content);
    left_layout->addWidget(cursor_header);

    auto* cursor_panel = makePanel(left_content);
    auto* cursor_layout = new QHBoxLayout(cursor_panel);
    cursor_layout->setContentsMargins(14, 10, 14, 10);
    cursor_layout->setSpacing(8);
    cursor_check_ = new QCheckBox("Capture mouse cursor", cursor_panel);
    cursor_check_->setChecked(true);
    cursor_layout->addWidget(cursor_check_);
    cursor_layout->addStretch(1);
    left_layout->addWidget(cursor_panel);

    left_layout->addStretch(1);
    scroll->setWidget(left_content);
    page_layout->addWidget(scroll, 1);

    rail_widget_ = new QWidget(this);
    rail_widget_->setObjectName("videoEffectiveRail");
    rail_widget_->setFixedWidth(260);
    auto* rail_layout = new QVBoxLayout(rail_widget_);
    rail_layout->setContentsMargins(14, 14, 14, 14);
    rail_layout->setSpacing(10);

    auto* rail_head = new QWidget(rail_widget_);
    auto* rail_head_layout = new QHBoxLayout(rail_head);
    rail_head_layout->setContentsMargins(0, 0, 0, 0);
    rail_head_layout->setSpacing(6);
    rail_head_layout->addWidget(makeLabel("EFFECTIVE OUTPUT", "videoRailHead", rail_head));
    rail_head_layout->addStretch(1);
    rail_head_layout->addWidget(makeLabel("UNSAVED", "videoRailUnsaved", rail_head));
    rail_layout->addWidget(rail_head);

    auto* output_panel = makePanel(rail_widget_);
    auto* output_layout = new QVBoxLayout(output_panel);
    output_layout->setContentsMargins(12, 12, 12, 12);
    output_layout->setSpacing(8);

    auto* kv_layout = new QGridLayout();
    kv_layout->setContentsMargins(0, 0, 0, 0);
    kv_layout->setHorizontalSpacing(10);
    kv_layout->setVerticalSpacing(5);
    kv_layout->setColumnStretch(0, 0);
    kv_layout->setColumnStretch(1, 1);
    addKvRow(kv_layout, output_panel, 0, "ENCODER", "NVENC AV1", "videoKvValueAccent");
    addKvRow(kv_layout, output_panel, 1, "PRESET", "P4 (balanced)", "videoKvValue");
    addKvRow(kv_layout, output_panel, 2, "RATE CTRL", "CQP", "videoKvValue");
    addKvRow(kv_layout, output_panel, 3, "CQ", "24", "videoKvValue");
    addKvRow(kv_layout, output_panel, 4, "FRAME RATE", "CFR 60.00", "videoKvValue");
    addKvRow(kv_layout, output_panel, 5, "RESOLUTION", "2560×1440", "videoKvValue");
    addKvRow(kv_layout, output_panel, 6, "GOP", "60", "videoKvValue");
    addKvRow(kv_layout, output_panel, 7, "B-FRAMES", "0", "videoKvValue");
    addKvRow(kv_layout, output_panel, 8, "CURSOR", "Captured", "videoKvValue");
    output_layout->addLayout(kv_layout);

    output_layout->addWidget(makeDivider(output_panel));

    auto* estimate_layout = new QGridLayout();
    estimate_layout->setContentsMargins(0, 0, 0, 0);
    estimate_layout->setHorizontalSpacing(10);
    estimate_layout->setVerticalSpacing(5);
    estimate_layout->setColumnStretch(0, 0);
    estimate_layout->setColumnStretch(1, 1);
    addKvRow(estimate_layout, output_panel, 0, "EST. BITRATE", "~38 Mb/s", "videoKvValueAccent");
    addKvRow(estimate_layout, output_panel, 1, "EST. SIZE/H", "~17.1 GB", "videoKvValue");
    output_layout->addLayout(estimate_layout);

    rail_layout->addWidget(output_panel);

    auto* compatibility_panel = makePanel(rail_widget_);
    auto* compatibility_layout = new QVBoxLayout(compatibility_panel);
    compatibility_layout->setContentsMargins(12, 12, 12, 12);
    compatibility_layout->setSpacing(8);

    auto* compatibility_head = new QWidget(compatibility_panel);
    auto* compatibility_head_layout = new QHBoxLayout(compatibility_head);
    compatibility_head_layout->setContentsMargins(0, 0, 0, 0);
    compatibility_head_layout->setSpacing(8);
    compatibility_head_layout->addWidget(makeLabel("COMPATIBILITY", "videoRailHead", compatibility_head));
    compatibility_head_layout->addStretch(1);
    auto* verified_pill = new ui::widgets::StatusPill(compatibility_head);
    verified_pill->setTone(ui::widgets::StatusPill::Tone::Ready);
    verified_pill->setText("VERIFIED");
    compatibility_head_layout->addWidget(verified_pill, 0, Qt::AlignRight | Qt::AlignVCenter);
    compatibility_layout->addWidget(compatibility_head);

    auto* compatibility_note = makeLabel("Detected GPU supports NVENC AV1. Driver 55.85 meets minimum.",
                                         "videoCompatNote", compatibility_panel);
    compatibility_note->setWordWrap(true);
    compatibility_layout->addWidget(compatibility_note);

    rail_layout->addWidget(compatibility_panel);
    rail_layout->addStretch(1);
    page_layout->addWidget(rail_widget_, 0);
}

void VideoPage::selectCodecCard(ui::widgets::CodecCard* selected_card) {
    for (auto* card : codec_cards_)
        card->setSelected(card == selected_card);
}

} // namespace exosnap
