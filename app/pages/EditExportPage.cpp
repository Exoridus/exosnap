#include "EditExportPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace exosnap {

using P = ui::theme::ExoSnapPalette;
using M = ui::theme::ExoSnapMetrics;
using namespace exosnap::ui::theme;

EditExportPage::EditExportPage(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void EditExportPage::buildUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ---- Mode-Bar ----
    auto* mode_bar = new QFrame(this);
    mode_bar->setObjectName(QStringLiteral("editExportModeBar"));
    mode_bar->setFixedHeight(52);
    mode_bar->setStyleSheet(QStringLiteral("QFrame#editExportModeBar {"
                                           "background:%1;"
                                           "border-bottom: 1px solid %2;"
                                           "}")
                                .arg(ActiveTheme().surf, ActiveTheme().line));

    auto* mode_bar_layout = new QHBoxLayout(mode_bar);
    mode_bar_layout->setContentsMargins(M::kSpaceMd, 0, M::kSpaceMd, 0);
    mode_bar_layout->setSpacing(M::kSpaceSm);

    back_btn_ = new QPushButton(QStringLiteral("←"), mode_bar);
    back_btn_->setObjectName(QStringLiteral("editExportBackBtn"));
    back_btn_->setFixedSize(32, 32);
    back_btn_->setProperty("role", "ghost");
    back_btn_->setToolTip(QStringLiteral("Back to Record"));

    title_label_ = new QLabel(QStringLiteral("Edit & export"), mode_bar);
    title_label_->setObjectName(QStringLiteral("editExportTitle"));
    title_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:14px; }").arg(ActiveTheme().ink));

    filename_label_ = new QLabel(this);
    filename_label_->setObjectName(QStringLiteral("editExportFilename"));
    filename_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));
    filename_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    mode_bar_layout->addWidget(back_btn_);
    mode_bar_layout->addWidget(title_label_);
    mode_bar_layout->addWidget(filename_label_, 1);
    mode_bar_layout->addStretch();

    secondary_action_btn_ = new QPushButton(mode_bar);
    secondary_action_btn_->setObjectName(QStringLiteral("editExportSecondaryBtn"));
    secondary_action_btn_->setProperty("role", "ghost");
    secondary_action_btn_->hide();

    primary_action_btn_ = new QPushButton(QStringLiteral("Export"), mode_bar);
    primary_action_btn_->setObjectName(QStringLiteral("editExportPrimaryBtn"));
    primary_action_btn_->setProperty("role", "primary");

    mode_bar_layout->addWidget(secondary_action_btn_);
    mode_bar_layout->addWidget(primary_action_btn_);

    root_layout->addWidget(mode_bar);

    // ---- Phase Stepper ----
    stepper_widget_ = new QWidget(this);
    stepper_widget_->setObjectName(QStringLiteral("editExportStepper"));
    stepper_widget_->setFixedHeight(40);
    stepper_widget_->setStyleSheet(QStringLiteral("QWidget#editExportStepper {"
                                                  "background:%1;"
                                                  "border-bottom: 1px solid %2;"
                                                  "}")
                                       .arg(ActiveTheme().surf, ActiveTheme().line));

    auto* stepper_layout = new QHBoxLayout(stepper_widget_);
    stepper_layout->setContentsMargins(M::kSpaceLg, 0, M::kSpaceLg, 0);
    stepper_layout->setSpacing(24);

    const auto makeStep = [&](const QString& text) -> QLabel* {
        auto* lbl = new QLabel(text, stepper_widget_);
        // Initial style: inactive (refreshPhase() will set the active one).
        lbl->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().dim));
        return lbl;
    };

    stepper_review_lbl_ = makeStep(QStringLiteral("Review"));
    stepper_edit_lbl_ = makeStep(QStringLiteral("Edit"));
    stepper_output_lbl_ = makeStep(QStringLiteral("Output"));

    stepper_layout->addWidget(stepper_review_lbl_);
    stepper_layout->addWidget(stepper_edit_lbl_);
    stepper_layout->addWidget(stepper_output_lbl_);
    stepper_layout->addStretch();

    root_layout->addWidget(stepper_widget_);

    // ---- 0.11 Placeholder Banner ----
    placeholder_banner_ = new QFrame(this);
    placeholder_banner_->setObjectName(QStringLiteral("editExportPlaceholderBanner"));
    placeholder_banner_->setStyleSheet(QStringLiteral("QFrame#editExportPlaceholderBanner {"
                                                      "background: %1;"
                                                      "border: 1px dashed %2;"
                                                      "border-radius: %3px;"
                                                      "margin: %4px;"
                                                      "}")
                                           .arg(ThemeRgba(QColor(QString::fromUtf8(ActiveTheme().caution)), 0.10),
                                                ThemeRgba(QColor(QString::fromUtf8(ActiveTheme().caution)), 0.40))
                                           .arg(M::kRadiusSm)
                                           .arg(M::kSpaceMd));

    auto* banner_layout = new QHBoxLayout(placeholder_banner_);
    banner_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);

    auto* banner_label = new QLabel(
        QStringLiteral("Editing & export tools arrive in 0.11 — this surface is a preview."), placeholder_banner_);
    banner_label->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().caution));
    banner_label->setWordWrap(true);
    banner_layout->addWidget(banner_label);

    root_layout->addWidget(placeholder_banner_);

    // ---- Main Content Area (splitter-like HBox) ----
    auto* content_area = new QWidget(this);
    auto* content_layout = new QHBoxLayout(content_area);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    // ---- Left pane (player + edit + output + exporting + result panels) ----
    auto* left_scroll = new QScrollArea(content_area);
    left_scroll->setWidgetResizable(true);
    left_scroll->setFrameShape(QFrame::NoFrame);
    left_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* left_widget = new QWidget(left_scroll);
    auto* left_layout = new QVBoxLayout(left_widget);
    left_layout->setContentsMargins(M::kSpaceMd, M::kSpaceMd, M::kSpaceSm, M::kSpaceMd);
    left_layout->setSpacing(M::kSpaceMd);

    // Player Frame
    player_frame_ = new QFrame(left_widget);
    player_frame_->setObjectName(QStringLiteral("editExportPlayer"));
    player_frame_->setMinimumHeight(220);
    player_frame_->setStyleSheet(QStringLiteral("QFrame#editExportPlayer {"
                                                "background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                                                "stop:0 #1a1a1e, stop:1 #0e0e10);"
                                                "border: 1px solid %1;"
                                                "border-radius: %2px;"
                                                "}")
                                     .arg(ActiveTheme().line2)
                                     .arg(M::kRadiusLg));

    auto* player_layout = new QVBoxLayout(player_frame_);
    player_layout->setAlignment(Qt::AlignCenter);

    player_icon_label_ = new QLabel(QStringLiteral("▶"), player_frame_);
    player_icon_label_->setObjectName(QStringLiteral("editExportPlayerIcon"));
    player_icon_label_->setAlignment(Qt::AlignCenter);
    player_icon_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:36px; }").arg(ActiveTheme().dim));

    auto* player_sub = new QLabel(QStringLiteral("Preview playback — coming in 0.11"), player_frame_);
    player_sub->setAlignment(Qt::AlignCenter);
    player_sub->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().dim));

    player_meta_label_ = new QLabel(this);
    player_meta_label_->setObjectName(QStringLiteral("editExportPlayerMeta"));
    player_meta_label_->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    player_meta_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:10px; }").arg(ActiveTheme().dim));

    player_layout->addStretch();
    player_layout->addWidget(player_icon_label_);
    player_layout->addWidget(player_sub);
    player_layout->addStretch();
    player_layout->addWidget(player_meta_label_);

    left_layout->addWidget(player_frame_);

    // Edit Controls
    edit_controls_ = new QWidget(left_widget);
    edit_controls_->setObjectName(QStringLiteral("editExportEditControls"));
    auto* edit_ctrl_layout = new QHBoxLayout(edit_controls_);
    edit_ctrl_layout->setContentsMargins(0, 0, 0, 0);
    edit_ctrl_layout->setSpacing(M::kSpaceSm);

    duration_label_ = new QLabel(QStringLiteral("0:00 / 0:00"), edit_controls_);
    duration_label_->setObjectName(QStringLiteral("editExportDurationLabel"));
    duration_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));

    trim_btn_ = new QPushButton(QStringLiteral("Trim"), edit_controls_);
    trim_btn_->setObjectName(QStringLiteral("editExportTrimBtn"));
    trim_btn_->setProperty("role", "ghost");
    trim_btn_->setEnabled(false);

    add_marker_btn_ = new QPushButton(QStringLiteral("Add Marker"), edit_controls_);
    add_marker_btn_->setObjectName(QStringLiteral("editExportAddMarkerBtn"));
    add_marker_btn_->setProperty("role", "ghost");
    add_marker_btn_->setEnabled(false);

    split_chapter_btn_ = new QPushButton(QStringLiteral("Split Chapter"), edit_controls_);
    split_chapter_btn_->setObjectName(QStringLiteral("editExportSplitChapterBtn"));
    split_chapter_btn_->setProperty("role", "ghost");
    split_chapter_btn_->setEnabled(false);

    edit_ctrl_layout->addWidget(duration_label_);
    edit_ctrl_layout->addStretch();
    edit_ctrl_layout->addWidget(trim_btn_);
    edit_ctrl_layout->addWidget(add_marker_btn_);
    edit_ctrl_layout->addWidget(split_chapter_btn_);

    left_layout->addWidget(edit_controls_);

    // Timeline
    timeline_frame_ = new QFrame(left_widget);
    timeline_frame_->setObjectName(QStringLiteral("editTimeline"));
    timeline_frame_->setFixedHeight(56);
    timeline_frame_->setEnabled(false);
    timeline_frame_->setStyleSheet(QStringLiteral("QFrame#editTimeline {"
                                                  "background:%1;"
                                                  "border: 1px solid %2;"
                                                  "border-radius: %3px;"
                                                  "}")
                                       .arg(ActiveTheme().raise, ActiveTheme().line)
                                       .arg(M::kRadiusSm));

    auto* timeline_layout = new QVBoxLayout(timeline_frame_);
    timeline_layout->setContentsMargins(M::kSpaceSm, 4, M::kSpaceSm, 4);
    timeline_layout->setSpacing(4);

    // Mini waveform simulation: 36 small frames
    auto* waveform_row = new QWidget(timeline_frame_);
    auto* waveform_layout = new QHBoxLayout(waveform_row);
    waveform_layout->setContentsMargins(0, 0, 0, 0);
    waveform_layout->setSpacing(2);
    for (int i = 0; i < 36; ++i) {
        auto* bar = new QFrame(waveform_row);
        bar->setFixedWidth(3);
        // Varying heights to simulate waveform
        const int height = 6 + ((i * 7 + 3) % 16);
        bar->setFixedHeight(height);
        bar->setStyleSheet(QStringLiteral("QFrame { background: %1; border-radius: 1px; }").arg(ActiveTheme().line2));
        waveform_layout->addWidget(bar);
    }
    waveform_layout->addStretch();

    auto* timeline_labels_row = new QWidget(timeline_frame_);
    auto* tl_labels_layout = new QHBoxLayout(timeline_labels_row);
    tl_labels_layout->setContentsMargins(0, 0, 0, 0);
    tl_labels_layout->setSpacing(0);

    timeline_in_label_ = new QLabel(QStringLiteral("In 0:00"), timeline_labels_row);
    timeline_in_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:10px; }").arg(ActiveTheme().dim));
    timeline_out_label_ = new QLabel(QStringLiteral("Out --:--"), timeline_labels_row);
    timeline_out_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:10px; }").arg(ActiveTheme().dim));

    tl_labels_layout->addWidget(timeline_in_label_);
    tl_labels_layout->addStretch();
    tl_labels_layout->addWidget(timeline_out_label_);

    timeline_layout->addWidget(waveform_row);
    timeline_layout->addWidget(timeline_labels_row);

    left_layout->addWidget(timeline_frame_);

    // Output Panel (3 option cards)
    output_panel_ = new QWidget(left_widget);
    output_panel_->setObjectName(QStringLiteral("editExportOutputPanel"));
    auto* output_panel_layout = new QVBoxLayout(output_panel_);
    output_panel_layout->setContentsMargins(0, 0, 0, 0);
    output_panel_layout->setSpacing(M::kSpaceSm);

    auto* output_title = new QLabel(QStringLiteral("Output format"), output_panel_);
    output_title->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(ActiveTheme().ink));
    output_panel_layout->addWidget(output_title);

    const auto makeOutputCard = [&](const QString& title, const QString& badge, const QString& badge_color,
                                    const QString& detail, bool selected) -> QFrame* {
        auto* card = new QFrame(output_panel_);
        card->setProperty("selected", selected);
        card->setStyleSheet(selected ? QStringLiteral("QFrame {"
                                                      "background:%1;"
                                                      "border: 1px solid %2;"
                                                      "border-radius: %3px;"
                                                      "}")
                                           .arg(ActiveTheme().raise, ActiveTheme().ac)
                                           .arg(M::kRadiusMd)
                                     : QStringLiteral("QFrame {"
                                                      "background:%1;"
                                                      "border: 1px solid %2;"
                                                      "border-radius: %3px;"
                                                      "}")
                                           .arg(ActiveTheme().surf2, ActiveTheme().line)
                                           .arg(M::kRadiusMd));

        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
        card_layout->setSpacing(4);

        auto* top_row = new QWidget(card);
        auto* top_layout = new QHBoxLayout(top_row);
        top_layout->setContentsMargins(0, 0, 0, 0);
        top_layout->setSpacing(M::kSpaceSm);

        auto* title_lbl = new QLabel(title, top_row);
        title_lbl->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(ActiveTheme().ink));

        auto* badge_lbl = new QLabel(badge, top_row);
        badge_lbl->setStyleSheet(QStringLiteral("QLabel {"
                                                "color:%1;"
                                                "background: rgba(%2, 0.15);"
                                                "border: 1px solid rgba(%2, 0.35);"
                                                "border-radius: 3px;"
                                                "padding: 1px 5px;"
                                                "font-size: 10px;"
                                                "}")
                                     .arg(badge_color, badge_color));

        top_layout->addWidget(title_lbl);
        top_layout->addWidget(badge_lbl);
        top_layout->addStretch();

        auto* detail_lbl = new QLabel(detail, card);
        detail_lbl->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

        card_layout->addWidget(top_row);
        card_layout->addWidget(detail_lbl);
        return card;
    };

    output_opt_keep_mkv_ = makeOutputCard(QStringLiteral("Keep MKV"), QStringLiteral("stream-copy"),
                                          QString::fromLatin1(ActiveTheme().success),
                                          QStringLiteral("Stream-copy \xc2\xb7 instant \xc2\xb7 lossless"), true);
    output_opt_keep_mkv_->setObjectName(QStringLiteral("editOutputOptKeepMkv"));

    output_opt_remux_mp4_ = makeOutputCard(
        QStringLiteral("Remux to MP4"), QStringLiteral("stream-copy"), QString::fromLatin1(ActiveTheme().success),
        QStringLiteral("Stream-copy AV1+Opus \xe2\x86\x92 MP4 \xc2\xb7 instant \xc2\xb7 lossless (ADR 0014)"), false);
    output_opt_remux_mp4_->setObjectName(QStringLiteral("editOutputOptRemuxMp4"));

    output_opt_reencode_ = makeOutputCard(QStringLiteral("MP4 \xc2\xb7 H.264 + AAC"), QStringLiteral("re-encode"),
                                          QString::fromLatin1(ActiveTheme().caution),
                                          QStringLiteral("Re-encode \xc2\xb7 ~3 min \xc2\xb7 quality cost"), false);
    output_opt_reencode_->setObjectName(QStringLiteral("editOutputOptReencode"));

    output_panel_layout->addWidget(output_opt_keep_mkv_);
    output_panel_layout->addWidget(output_opt_remux_mp4_);
    output_panel_layout->addWidget(output_opt_reencode_);

    // Destination row
    auto* dest_row = new QWidget(output_panel_);
    auto* dest_layout = new QHBoxLayout(dest_row);
    dest_layout->setContentsMargins(0, 0, 0, 0);
    dest_layout->setSpacing(M::kSpaceSm);

    auto* dest_lbl_title = new QLabel(QStringLiteral("Destination:"), dest_row);
    dest_lbl_title->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));

    dest_folder_label_ = new QLabel(QStringLiteral("Same folder as source"), dest_row);
    dest_folder_label_->setObjectName(QStringLiteral("editExportDestFolder"));
    dest_folder_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ThemeText1Color(ActiveTheme())));

    browse_dest_btn_ = new QPushButton(QStringLiteral("Browse…"), dest_row);
    browse_dest_btn_->setProperty("role", "ghost");
    browse_dest_btn_->setEnabled(false);

    dest_layout->addWidget(dest_lbl_title);
    dest_layout->addWidget(dest_folder_label_, 1);
    dest_layout->addWidget(browse_dest_btn_);

    output_panel_layout->addWidget(dest_row);

    left_layout->addWidget(output_panel_);

    // Exporting Panel
    exporting_panel_ = new QWidget(left_widget);
    exporting_panel_->setObjectName(QStringLiteral("editExportExportingPanel"));
    auto* exporting_layout = new QVBoxLayout(exporting_panel_);
    exporting_layout->setContentsMargins(0, 0, 0, 0);
    exporting_layout->setSpacing(M::kSpaceSm);

    exporting_status_label_ = new QLabel(QStringLiteral("Exporting…"), exporting_panel_);
    exporting_status_label_->setObjectName(QStringLiteral("editExportExportingStatus"));
    exporting_status_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:14px; }").arg(ActiveTheme().ink));

    exporting_bar_ = new QProgressBar(exporting_panel_);
    exporting_bar_->setObjectName(QStringLiteral("editExportProgressBar"));
    exporting_bar_->setRange(0, 100);
    exporting_bar_->setValue(62);
    exporting_bar_->setFixedHeight(6);
    exporting_bar_->setTextVisible(false);
    exporting_bar_->setStyleSheet(QStringLiteral("QProgressBar { background:%1; border-radius:3px; border:none; }"
                                                 "QProgressBar::chunk { background:%2; border-radius:3px; }")
                                      .arg(ActiveTheme().raise, ActiveTheme().ac));

    exporting_detail_label_ = new QLabel(QStringLiteral("Stream-copy \xc2\xb7 no quality loss"), exporting_panel_);
    exporting_detail_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));

    exporting_layout->addWidget(exporting_status_label_);
    exporting_layout->addWidget(exporting_bar_);
    exporting_layout->addWidget(exporting_detail_label_);

    left_layout->addWidget(exporting_panel_);

    // Result Panel (Done / Failed)
    result_panel_ = new QWidget(left_widget);
    result_panel_->setObjectName(QStringLiteral("editExportResultPanel"));
    auto* result_layout = new QVBoxLayout(result_panel_);
    result_layout->setContentsMargins(0, 0, 0, 0);
    result_layout->setSpacing(M::kSpaceSm);

    result_title_label_ = new QLabel(result_panel_);
    result_title_label_->setObjectName(QStringLiteral("editExportResultTitle"));
    result_title_label_->setStyleSheet(QStringLiteral("QLabel { font-weight:600; font-size:16px; }"));

    result_detail_label_ = new QLabel(result_panel_);
    result_detail_label_->setObjectName(QStringLiteral("editExportResultDetail"));
    result_detail_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));
    result_detail_label_->setWordWrap(true);

    auto* result_actions_row = new QWidget(result_panel_);
    auto* result_actions_layout = new QHBoxLayout(result_actions_row);
    result_actions_layout->setContentsMargins(0, 0, 0, 0);
    result_actions_layout->setSpacing(M::kSpaceSm);

    result_open_folder_btn_ = new QPushButton(QStringLiteral("Open folder"), result_actions_row);
    result_open_folder_btn_->setProperty("role", "ghost");

    result_reveal_btn_ = new QPushButton(QStringLiteral("Reveal file"), result_actions_row);
    result_reveal_btn_->setProperty("role", "ghost");

    result_actions_layout->addWidget(result_open_folder_btn_);
    result_actions_layout->addWidget(result_reveal_btn_);
    result_actions_layout->addStretch();

    result_layout->addWidget(result_title_label_);
    result_layout->addWidget(result_detail_label_);
    result_layout->addWidget(result_actions_row);

    left_layout->addWidget(result_panel_);
    left_layout->addStretch();

    left_scroll->setWidget(left_widget);

    // ---- Right pane: Detail Rail ----
    detail_rail_ = new QFrame(content_area);
    detail_rail_->setObjectName(QStringLiteral("editExportDetailRail"));
    detail_rail_->setFixedWidth(220);
    detail_rail_->setStyleSheet(QStringLiteral("QFrame#editExportDetailRail {"
                                               "background:%1;"
                                               "border-left: 1px solid %2;"
                                               "}")
                                    .arg(ActiveTheme().surf, ActiveTheme().line));

    auto* rail_layout = new QVBoxLayout(detail_rail_);
    rail_layout->setContentsMargins(M::kSpaceMd, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    rail_layout->setSpacing(10);

    auto* rail_title = new QLabel(QStringLiteral("Recording info"), detail_rail_);
    rail_title->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:11px; text-transform:uppercase; }")
            .arg(ActiveTheme().dim));
    rail_layout->addWidget(rail_title);

    auto* rail_sep = new QFrame(detail_rail_);
    rail_sep->setFrameShape(QFrame::HLine);
    rail_sep->setStyleSheet(QStringLiteral("QFrame { color:%1; }").arg(ActiveTheme().line));
    rail_layout->addWidget(rail_sep);

    const auto makeFactRow = [&](const QString& key_text, QLabel*& val_label_ref) {
        auto* row = new QWidget(detail_rail_);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(M::kSpaceSm);

        auto* key = new QLabel(key_text, row);
        key->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().dim));
        key->setFixedWidth(70);

        val_label_ref = new QLabel(QStringLiteral("–"), row);
        val_label_ref->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ThemeText1Color(ActiveTheme())));
        val_label_ref->setWordWrap(true);

        row_layout->addWidget(key);
        row_layout->addWidget(val_label_ref, 1);
        rail_layout->addWidget(row);
    };

    makeFactRow(QStringLiteral("Duration"), fact_duration_val_);
    fact_duration_val_->setObjectName(QStringLiteral("editFactDuration"));

    makeFactRow(QStringLiteral("Size"), fact_size_val_);
    fact_size_val_->setObjectName(QStringLiteral("editFactSize"));

    makeFactRow(QStringLiteral("Resolution"), fact_res_val_);
    fact_res_val_->setObjectName(QStringLiteral("editFactResolution"));

    makeFactRow(QStringLiteral("Frame rate"), fact_fps_val_);
    fact_fps_val_->setObjectName(QStringLiteral("editFactFps"));

    makeFactRow(QStringLiteral("Video"), fact_video_val_);
    fact_video_val_->setObjectName(QStringLiteral("editFactVideo"));

    makeFactRow(QStringLiteral("Audio"), fact_audio_val_);
    fact_audio_val_->setObjectName(QStringLiteral("editFactAudio"));

    makeFactRow(QStringLiteral("Container"), fact_container_val_);
    fact_container_val_->setObjectName(QStringLiteral("editFactContainer"));

    rail_layout->addStretch();

    content_layout->addWidget(left_scroll, 1);
    content_layout->addWidget(detail_rail_);

    root_layout->addWidget(content_area, 1);

    // Wire signals
    connect(back_btn_, &QPushButton::clicked, this, &EditExportPage::onBackClicked);
    connect(primary_action_btn_, &QPushButton::clicked, this, [this]() {
        switch (phase_) {
        case Phase::Output:
            onExportClicked();
            break;
        case Phase::Exporting:
            onCancelExportClicked();
            break;
        case Phase::Done:
            onDoneClicked();
            break;
        case Phase::Failed:
            onRetryExportClicked();
            break;
        default:
            setPhase(Phase::Output);
            break;
        }
    });
    connect(secondary_action_btn_, &QPushButton::clicked, this, &EditExportPage::onOpenFolderClicked);
    connect(result_open_folder_btn_, &QPushButton::clicked, this, &EditExportPage::onOpenFolderClicked);
    connect(result_reveal_btn_, &QPushButton::clicked, this, &EditExportPage::onRevealFileClicked);

    // Initial phase
    refreshPhase();
}

void EditExportPage::setRecordingInfo(const QString& file_path, const QString& duration, const QString& size,
                                      const QString& resolution, const QString& fps, const QString& video_codec,
                                      const QString& audio_codec, const QString& container) {
    file_path_ = file_path;
    duration_ = duration;
    size_ = size;
    resolution_ = resolution;
    fps_ = fps;
    video_codec_ = video_codec;
    audio_codec_ = audio_codec;
    container_ = container;

    // Update filename label
    if (filename_label_) {
        const int sep = qMax(file_path.lastIndexOf(QLatin1Char('/')), file_path.lastIndexOf(QLatin1Char('\\')));
        filename_label_->setText(sep >= 0 ? file_path.mid(sep + 1) : file_path);
    }

    // Update detail rail
    if (fact_duration_val_)
        fact_duration_val_->setText(duration_.isEmpty() ? QStringLiteral("–") : duration_);
    if (fact_size_val_)
        fact_size_val_->setText(size_.isEmpty() ? QStringLiteral("–") : size_);
    if (fact_res_val_)
        fact_res_val_->setText(resolution_.isEmpty() ? QStringLiteral("–") : resolution_);
    if (fact_fps_val_)
        fact_fps_val_->setText(fps_.isEmpty() ? QStringLiteral("–") : fps_);
    if (fact_video_val_)
        fact_video_val_->setText(video_codec_.isEmpty() ? QStringLiteral("–") : video_codec_);
    if (fact_audio_val_)
        fact_audio_val_->setText(audio_codec_.isEmpty() ? QStringLiteral("–") : audio_codec_);
    if (fact_container_val_)
        fact_container_val_->setText(container_.isEmpty() ? QStringLiteral("–") : container_);

    // Update player meta
    if (player_meta_label_)
        player_meta_label_->setText(QStringLiteral("%1  %2  %3").arg(resolution_, fps_, container_));

    // Update timeline labels
    if (timeline_out_label_)
        timeline_out_label_->setText(QStringLiteral("Out %1").arg(duration_));

    // Update edit controls duration
    if (duration_label_)
        duration_label_->setText(QStringLiteral("0:00 / %1").arg(duration_));
}

void EditExportPage::setPhase(Phase phase) {
    phase_ = phase;
    refreshPhase();
}

void EditExportPage::refreshPhase() {
    // ---- Stepper highlight ----
    // Map phases to the logical stepper step (Review/Edit → "Review" active,
    // Output/Exporting → "Output" active, Done/Failed → "Output" stays active).
    const auto stepStyle = [&](bool active) -> QString {
        return active ? QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; "
                                       "border-bottom: 2px solid %1; padding-bottom:2px; }")
                            .arg(ActiveTheme().ac)
                      : QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().dim);
    };
    const bool step_review = (phase_ == Phase::Review);
    const bool step_edit = (phase_ == Phase::Edit);
    const bool step_output =
        (phase_ == Phase::Output || phase_ == Phase::Exporting || phase_ == Phase::Done || phase_ == Phase::Failed);
    if (stepper_review_lbl_)
        stepper_review_lbl_->setStyleSheet(stepStyle(step_review));
    if (stepper_edit_lbl_)
        stepper_edit_lbl_->setStyleSheet(stepStyle(step_edit));
    if (stepper_output_lbl_)
        stepper_output_lbl_->setStyleSheet(stepStyle(step_output));

    // ---- Show/hide panels ----
    const bool show_player = (phase_ == Phase::Review || phase_ == Phase::Edit);
    const bool show_edit = (phase_ == Phase::Review || phase_ == Phase::Edit);
    const bool show_timeline = (phase_ == Phase::Review || phase_ == Phase::Edit);
    const bool show_output = (phase_ == Phase::Output);
    const bool show_exporting = (phase_ == Phase::Exporting);
    const bool show_result = (phase_ == Phase::Done || phase_ == Phase::Failed);

    if (player_frame_)
        player_frame_->setVisible(show_player);
    if (edit_controls_)
        edit_controls_->setVisible(show_edit);
    if (timeline_frame_)
        timeline_frame_->setVisible(show_timeline);
    if (output_panel_)
        output_panel_->setVisible(show_output);
    if (exporting_panel_)
        exporting_panel_->setVisible(show_exporting);
    if (result_panel_)
        result_panel_->setVisible(show_result);

    // Update primary/secondary buttons
    if (!primary_action_btn_ || !secondary_action_btn_)
        return;

    secondary_action_btn_->hide();

    switch (phase_) {
    case Phase::Review:
        primary_action_btn_->setText(QStringLiteral("Go to Output"));
        primary_action_btn_->setProperty("role", "ghost");
        break;
    case Phase::Edit:
        primary_action_btn_->setText(QStringLiteral("Go to Output"));
        primary_action_btn_->setProperty("role", "ghost");
        break;
    case Phase::Output:
        primary_action_btn_->setText(QStringLiteral("Export"));
        primary_action_btn_->setProperty("role", "primary");
        break;
    case Phase::Exporting:
        primary_action_btn_->setText(QStringLiteral("Cancel"));
        primary_action_btn_->setProperty("role", "ghost");
        if (exporting_status_label_)
            exporting_status_label_->setText(QStringLiteral("Exporting…"));
        break;
    case Phase::Done:
        primary_action_btn_->setText(QStringLiteral("Done"));
        primary_action_btn_->setProperty("role", "primary");
        secondary_action_btn_->setText(QStringLiteral("Open folder"));
        secondary_action_btn_->show();
        if (result_title_label_) {
            result_title_label_->setText(QStringLiteral("Export complete"));
            result_title_label_->setStyleSheet(
                QStringLiteral("QLabel { color:%1; font-weight:600; font-size:16px; }").arg(ActiveTheme().success));
        }
        if (result_detail_label_)
            result_detail_label_->setText(QStringLiteral("Sprint-demo.mp4 \xc2\xb7 stream-copy \xc2\xb7 lossless"));
        break;
    case Phase::Failed:
        primary_action_btn_->setText(QStringLiteral("Retry export"));
        primary_action_btn_->setProperty("role", "primary");
        if (result_title_label_) {
            result_title_label_->setText(QStringLiteral("Export failed"));
            result_title_label_->setStyleSheet(
                QStringLiteral("QLabel { color:%1; font-weight:600; font-size:16px; }").arg(ActiveTheme().error));
        }
        if (result_detail_label_)
            result_detail_label_->setText(QStringLiteral("Export failed — disk full"));
        break;
    }

    // Force style refresh for property-driven QSS
    if (primary_action_btn_) {
        primary_action_btn_->style()->unpolish(primary_action_btn_);
        primary_action_btn_->style()->polish(primary_action_btn_);
    }
}

// ---- Slots ----

void EditExportPage::onBackClicked() {
    emit backRequested();
}

void EditExportPage::onExportClicked() {
    setPhase(Phase::Exporting);

    // Demo: after 1500ms switch to Done
    if (!export_demo_timer_) {
        export_demo_timer_ = new QTimer(this);
        export_demo_timer_->setSingleShot(true);
        connect(export_demo_timer_, &QTimer::timeout, this, [this]() {
            setPhase(Phase::Done);
            emit exportCompleted(QStringLiteral("Sprint-demo.mp4"));
        });
    }
    export_demo_timer_->start(1500);
}

void EditExportPage::onCancelExportClicked() {
    if (export_demo_timer_)
        export_demo_timer_->stop();
    setPhase(Phase::Output);
}

void EditExportPage::onDoneClicked() {
    emit backRequested();
}

void EditExportPage::onOpenFolderClicked() {
    // Stub — engine not wired yet
}

void EditExportPage::onRevealFileClicked() {
    // Stub — engine not wired yet
}

void EditExportPage::onRetryExportClicked() {
    setPhase(Phase::Exporting);
    if (!export_demo_timer_) {
        export_demo_timer_ = new QTimer(this);
        export_demo_timer_->setSingleShot(true);
        connect(export_demo_timer_, &QTimer::timeout, this, [this]() {
            setPhase(Phase::Done);
            emit exportCompleted(QStringLiteral("Sprint-demo.mp4"));
        });
    }
    export_demo_timer_->start(1500);
}

} // namespace exosnap
