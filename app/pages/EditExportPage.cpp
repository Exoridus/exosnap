#include "EditExportPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"

#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRectF>
#include <QSaveFile>
#include <QScrollArea>
#include <QSize>
#include <QStyle>
#include <QSvgRenderer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace exosnap {

using P = ui::theme::ExoSnapPalette;
using M = ui::theme::ExoSnapMetrics;
using namespace exosnap::ui::theme;

namespace {

// Lucide-style 24x24 stroke paths (subset shared with shared.jsx ICON_PATHS).
QByteArray editIconPathFor(const QString& key) {
    if (key == QLatin1String("chevLeft"))
        return QByteArrayLiteral("M14 5l-5 5 5 5");
    if (key == QLatin1String("play"))
        return QByteArrayLiteral("M6 4l14 8-14 8V4z");
    if (key == QLatin1String("checkCircle"))
        return QByteArrayLiteral("M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20zM8 12l3 3 5-6");
    if (key == QLatin1String("error"))
        return QByteArrayLiteral("M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20zM15 9l-6 6M9 9l6 6");
    return {};
}

// Render an icon to a crisp (2x) transparent pixmap, using the same QSvgRenderer
// stroke technique as AudioSourceToggle (fill:none, stroke:color, round caps).
QPixmap renderEditIcon(const QString& key, int px, const QColor& color) {
    const QByteArray path = editIconPathFor(key);
    if (path.isEmpty())
        return {};
    QByteArray svg;
    svg.reserve(256);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(color.name(QColor::HexRgb).toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='");
    svg.append(path);
    svg.append("'/></svg>");
    QSvgRenderer renderer(svg);

    constexpr qreal kDpr = 2.0;
    QPixmap pm(static_cast<int>(px * kDpr), static_cast<int>(px * kDpr));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p, QRectF(0, 0, px * kDpr, px * kDpr));
    p.end();
    pm.setDevicePixelRatio(kDpr);
    return pm;
}

QColor themeColor(const char* css) {
    return QColor(QString::fromUtf8(css));
}

// Derived alpha tokens (mirrors BuildTokens() in ExoSnapTheme.cpp).
QString acDimToken() {
    const auto& t = ActiveTheme();
    return ThemeRgba(themeColor(t.ac), t.kind == ThemeKind::Dark ? 0.14 : 0.12);
}
QString acB2Token() {
    const auto& t = ActiveTheme();
    return ThemeRgba(themeColor(t.ac), t.kind == ThemeKind::Dark ? 0.60 : 0.52);
}
QString okDimToken() {
    const auto& t = ActiveTheme();
    return ThemeRgba(themeColor(t.success), t.kind == ThemeKind::Dark ? 0.13 : 0.12);
}
QString okBToken() {
    const auto& t = ActiveTheme();
    return ThemeRgba(themeColor(t.success), t.kind == ThemeKind::Dark ? 0.44 : 0.42);
}
QString errDimToken() {
    const auto& t = ActiveTheme();
    return ThemeRgba(themeColor(t.error), t.kind == ThemeKind::Dark ? 0.13 : 0.12);
}
QString errBToken() {
    const auto& t = ActiveTheme();
    return ThemeRgba(themeColor(t.error), t.kind == ThemeKind::Dark ? 0.44 : 0.42);
}

} // namespace

EditExportPage::EditExportPage(QWidget* parent) : QWidget(parent) {
    buildUi();
}

EditExportPage::~EditExportPage() {
    export_cancel_.store(true);
    if (export_thread_.joinable())
        export_thread_.join();
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

    back_btn_ = new QPushButton(mode_bar);
    back_btn_->setObjectName(QStringLiteral("editExportBackBtn"));
    back_btn_->setFixedSize(32, 32);
    back_btn_->setToolTip(QStringLiteral("Back to Record"));
    back_btn_->setCursor(Qt::PointingHandCursor);
    back_btn_->setIcon(QIcon(renderEditIcon(QStringLiteral("chevLeft"), 16, themeColor(ActiveTheme().mut))));
    back_btn_->setIconSize(QSize(16, 16));
    back_btn_->setStyleSheet(QStringLiteral("QPushButton#editExportBackBtn {"
                                            "background:%1;"
                                            "border: 1px solid %2;"
                                            "border-radius: 9px;"
                                            "}"
                                            "QPushButton#editExportBackBtn:hover { background:%3; }")
                                 .arg(ActiveTheme().surf2, ActiveTheme().line2, ActiveTheme().raise));

    title_label_ = new QLabel(QStringLiteral("Edit & export"), mode_bar);
    title_label_->setObjectName(QStringLiteral("editExportTitle"));
    title_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:700; font-size:16px; }").arg(ActiveTheme().ink));

    filename_label_ = new QLabel(this);
    filename_label_->setObjectName(QStringLiteral("editExportFilename"));
    filename_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-family:'IBM Plex Mono','Consolas',monospace; font-size:12.5px; }")
            .arg(ActiveTheme().ac));
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
    player_frame_->setMinimumHeight(180);
    // Enforce a 16:9 aspect ratio via eventFilter (Qt widgets have no native
    // height-for-width without a subclass).
    player_frame_->installEventFilter(this);
    player_frame_->setStyleSheet(QStringLiteral("QFrame#editExportPlayer {"
                                                "background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                                                "stop:0 #1a1a1e, stop:1 #0e0e10);"
                                                "border: 1px solid %1;"
                                                "border-radius: %2px;"
                                                "}")
                                     .arg(ActiveTheme().line)
                                     .arg(M::kRadiusLg));

    auto* player_layout = new QVBoxLayout(player_frame_);
    player_layout->setAlignment(Qt::AlignCenter);

    // 60px circular play button (stroke play-glyph 24px) instead of a text ▶.
    player_icon_label_ = new QLabel(player_frame_);
    player_icon_label_->setObjectName(QStringLiteral("editExportPlayerIcon"));
    player_icon_label_->setFixedSize(60, 60);
    player_icon_label_->setAlignment(Qt::AlignCenter);
    player_icon_label_->setPixmap(renderEditIcon(QStringLiteral("play"), 24, themeColor(ActiveTheme().ink)));
    player_icon_label_->setStyleSheet(QStringLiteral("QLabel#editExportPlayerIcon {"
                                                     "background: rgba(14, 14, 16, 0.7);"
                                                     "border: 1px solid %1;"
                                                     "border-radius: 30px;"
                                                     "}")
                                          .arg(ActiveTheme().line2));

    auto* player_sub = new QLabel(QStringLiteral("Preview playback — coming in 0.11"), player_frame_);
    player_sub->setAlignment(Qt::AlignCenter);
    player_sub->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().dim));

    player_meta_label_ = new QLabel(this);
    player_meta_label_->setObjectName(QStringLiteral("editExportPlayerMeta"));
    player_meta_label_->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    player_meta_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:10px; }").arg(ActiveTheme().dim));

    player_layout->addStretch();
    player_layout->addWidget(player_icon_label_, 0, Qt::AlignHCenter);
    player_layout->addWidget(player_sub);
    player_layout->addStretch();
    player_layout->addWidget(player_meta_label_);

    left_layout->addWidget(player_frame_);

    // Review Panel (post-flight report, shown only in Review phase)
    review_panel_ = new QWidget(left_widget);
    review_panel_->setObjectName(QStringLiteral("editExportReviewPanel"));
    auto* review_layout = new QVBoxLayout(review_panel_);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->setSpacing(M::kSpaceSm);

    auto* review_title = new QLabel(QStringLiteral("Post-recording report"), review_panel_);
    review_title->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(ActiveTheme().ink));

    review_drop_label_ = new QLabel(QStringLiteral("Frame drops: \xe2\x80\x93"), review_panel_);
    review_drop_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

    review_drift_label_ = new QLabel(QStringLiteral("Peak A/V drift: \xe2\x80\x93"), review_panel_);
    review_drift_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

    review_health_label_ = new QLabel(QStringLiteral("Pipeline health: \xe2\x80\x93"), review_panel_);
    review_health_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

    review_layout->addWidget(review_title);
    review_layout->addWidget(review_drop_label_);
    review_layout->addWidget(review_drift_label_);
    review_layout->addWidget(review_health_label_);
    left_layout->addWidget(review_panel_);

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
    timeline_frame_->setFixedHeight(52);
    timeline_frame_->setEnabled(false);
    timeline_frame_->setStyleSheet(QStringLiteral("QFrame#editTimeline {"
                                                  "background:%1;"
                                                  "border: 1px solid %2;"
                                                  "border-radius: %3px;"
                                                  "}")
                                       .arg(ActiveTheme().surf2, ActiveTheme().line)
                                       .arg(M::kRadiusMd));

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
        bar->setStyleSheet(QStringLiteral("QFrame { background: %1; border-radius: 1px; }")
                               .arg(ThemeRgba(themeColor(ActiveTheme().ac), 0.35)));
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

    // Output Panel (container + save-mode selectors)
    output_panel_ = new QWidget(left_widget);
    output_panel_->setObjectName(QStringLiteral("editExportOutputPanel"));
    auto* output_panel_layout = new QVBoxLayout(output_panel_);
    output_panel_layout->setContentsMargins(0, 0, 0, 0);
    output_panel_layout->setSpacing(M::kSpaceSm);

    auto* output_title = new QLabel(QStringLiteral("Output format"), output_panel_);
    output_title->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(ActiveTheme().ink));
    output_panel_layout->addWidget(output_title);

    // Container selection (stream-copy only — no re-encode per ADR-0014)
    auto* container_lbl = new QLabel(QStringLiteral("Container:"), output_panel_);
    container_lbl->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));
    output_panel_layout->addWidget(container_lbl);

    output_container_combo_ = new QComboBox(output_panel_);
    output_container_combo_->setObjectName(QStringLiteral("outputContainerCombo"));
    output_container_combo_->addItem(QStringLiteral("MKV  \xe2\x80\x93  stream-copy, lossless"), QStringLiteral("mkv"));
    output_container_combo_->addItem(QStringLiteral("MP4  \xe2\x80\x93  stream-copy, lossless (ADR\xc2\xa0"
                                                    "0014)"),
                                     QStringLiteral("mp4"));
    output_panel_layout->addWidget(output_container_combo_);

    // Save mode: new file or overwrite original
    auto* savemode_lbl = new QLabel(QStringLiteral("Save:"), output_panel_);
    savemode_lbl->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ActiveTheme().mut));
    output_panel_layout->addWidget(savemode_lbl);

    output_save_mode_combo_ = new QComboBox(output_panel_);
    output_save_mode_combo_->setObjectName(QStringLiteral("outputSaveModeCombo"));
    output_save_mode_combo_->addItem(QStringLiteral("Save as new file  (\xe2\x80\x9c<name>_edit.<ext>\xe2\x80\x9d)"),
                                     QStringLiteral("new"));
    output_save_mode_combo_->addItem(QStringLiteral("Overwrite original  (atomic replace)"),
                                     QStringLiteral("overwrite"));
    output_panel_layout->addWidget(output_save_mode_combo_);

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
    result_panel_->setAttribute(Qt::WA_StyledBackground, true);
    auto* result_layout = new QVBoxLayout(result_panel_);
    result_layout->setContentsMargins(M::kSpaceMd, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    result_layout->setSpacing(M::kSpaceSm);

    // Status badge — 72×72 circle hosting a 34px check/error glyph (set per phase).
    result_icon_label_ = new QLabel(result_panel_);
    result_icon_label_->setObjectName(QStringLiteral("editExportResultIcon"));
    result_icon_label_->setFixedSize(72, 72);
    result_icon_label_->setAlignment(Qt::AlignCenter);
    result_layout->addWidget(result_icon_label_, 0, Qt::AlignLeft);

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
    connect(trim_btn_, &QPushButton::clicked, this, &EditExportPage::onTrimClicked);
    connect(add_marker_btn_, &QPushButton::clicked, this, &EditExportPage::onAddMarkerClicked);
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

void EditExportPage::setEditContext(const EditContext& ctx) {
    ctx_ = ctx;
    setRecordingInfo(ctx_.output_path, ctx_.duration, ctx_.size, ctx_.resolution, ctx_.fps, ctx_.video_codec,
                     ctx_.audio_codec, ctx_.container);

    // --- Populate review panel ---
    const auto& snap = ctx_.completed_snapshot;
    const bool has_snap = snap.valid || snap.session_generation > 0;

    if (review_drop_label_) {
        const uint64_t total_dropped = snap.capture.frames_dropped_total();
        const uint64_t total_frames = snap.capture.frames_emitted + total_dropped;
        if (has_snap && total_frames > 0) {
            const double pct = 100.0 * static_cast<double>(total_dropped) / static_cast<double>(total_frames);
            review_drop_label_->setText(QStringLiteral("Frame drops: %1%").arg(pct, 0, 'f', 1));
        } else {
            review_drop_label_->setText(QStringLiteral("Frame drops: \xe2\x80\x93"));
        }
    }

    if (review_drift_label_) {
        if (ctx_.av_drift_available) {
            review_drift_label_->setText(
                QStringLiteral("Peak A/V drift: \xc2\xb1%1\xc2\xa0ms").arg(ctx_.peak_av_drift_ms, 0, 'f', 0));
        } else {
            review_drift_label_->setText(QStringLiteral("A/V drift: unavailable"));
        }
    }

    if (review_health_label_) {
        if (has_snap) {
            const char* health_str = "Unknown";
            switch (snap.health) {
            case recorder_core::PipelineHealth::Good:
                health_str = "Good";
                break;
            case recorder_core::PipelineHealth::Warning:
                health_str = "Warning";
                break;
            case recorder_core::PipelineHealth::Critical:
                health_str = "Critical";
                break;
            case recorder_core::PipelineHealth::Unavailable:
                health_str = "Unavailable";
                break;
            default:
                break;
            }
            review_health_label_->setText(QStringLiteral("Pipeline health: %1").arg(QLatin1String(health_str)));
        } else {
            review_health_label_->setText(QStringLiteral("Pipeline health: \xe2\x80\x93"));
        }
    }

    // --- Load keyframe timestamps from MKV master (background is fine; fast for short clips) ---
    keyframe_timestamps_.clear();
    trim_start_us_ = recorder_core::TrimRange::kNoTimestamp;
    trim_end_us_ = recorder_core::TrimRange::kNoTimestamp;
    if (!ctx_.mkv_master_path.isEmpty()) {
        keyframe_timestamps_ =
            recorder_core::ExtractKeyframeTimestamps(std::filesystem::path(ctx_.mkv_master_path.toStdWString()));
    }

    // --- Load markers from sidecar (falls back to session markers) ---
    loadMarkers();
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
    const bool show_review_panel = (phase_ == Phase::Review);
    const bool show_player = (phase_ == Phase::Review || phase_ == Phase::Edit);
    const bool show_edit = (phase_ == Phase::Edit);
    const bool show_timeline = (phase_ == Phase::Edit);
    const bool show_output = (phase_ == Phase::Output);
    const bool show_exporting = (phase_ == Phase::Exporting);
    const bool show_result = (phase_ == Phase::Done || phase_ == Phase::Failed);

    if (review_panel_)
        review_panel_->setVisible(show_review_panel);
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

    // Enable trim/marker buttons only in Edit phase
    if (trim_btn_)
        trim_btn_->setEnabled(phase_ == Phase::Edit);
    if (add_marker_btn_)
        add_marker_btn_->setEnabled(phase_ == Phase::Edit);

    // Update primary/secondary buttons
    if (!primary_action_btn_ || !secondary_action_btn_)
        return;

    secondary_action_btn_->hide();

    switch (phase_) {
    case Phase::Review:
        primary_action_btn_->setText(QStringLiteral("Continue to output"));
        primary_action_btn_->setProperty("role", "ghost");
        break;
    case Phase::Edit:
        primary_action_btn_->setText(QStringLiteral("Continue to output"));
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
        if (result_panel_)
            result_panel_->setStyleSheet(QStringLiteral("QWidget#editExportResultPanel {"
                                                        "background:%1;"
                                                        "border: 1px solid %2;"
                                                        "border-radius: 13px;"
                                                        "}")
                                             .arg(okDimToken(), okBToken()));
        if (result_icon_label_) {
            result_icon_label_->setPixmap(
                renderEditIcon(QStringLiteral("checkCircle"), 34, themeColor(ActiveTheme().success)));
            result_icon_label_->setStyleSheet(QStringLiteral("QLabel#editExportResultIcon {"
                                                             "background:%1;"
                                                             "border: 1px solid %2;"
                                                             "border-radius: 36px;"
                                                             "}")
                                                  .arg(okDimToken(), okBToken()));
        }
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
        if (result_panel_)
            result_panel_->setStyleSheet(QStringLiteral("QWidget#editExportResultPanel {"
                                                        "background:%1;"
                                                        "border: 1px solid %2;"
                                                        "border-radius: 13px;"
                                                        "}")
                                             .arg(errDimToken(), errBToken()));
        if (result_icon_label_) {
            result_icon_label_->setPixmap(renderEditIcon(QStringLiteral("error"), 34, themeColor(ActiveTheme().error)));
            result_icon_label_->setStyleSheet(QStringLiteral("QLabel#editExportResultIcon {"
                                                             "background:%1;"
                                                             "border: 1px solid %2;"
                                                             "border-radius: 36px;"
                                                             "}")
                                                  .arg(errDimToken(), errBToken()));
        }
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

bool EditExportPage::eventFilter(QObject* obj, QEvent* event) {
    // Keep the player area at a strict 16:9 ratio relative to its current width.
    if (obj == player_frame_ && event->type() == QEvent::Resize) {
        const int w = player_frame_->width();
        const int target = qRound(w * 9.0 / 16.0);
        if (target > 0 && player_frame_->height() != target)
            player_frame_->setFixedHeight(target);
    }
    return QWidget::eventFilter(obj, event);
}

// ---- Slots ----

void EditExportPage::onBackClicked() {
    emit backRequested();
}

void EditExportPage::onExportClicked() {
    runExport();
}

void EditExportPage::onCancelExportClicked() {
    export_cancel_.store(true);
    // The background thread will detect the cancel and stop; we snap back to Output immediately.
    setPhase(Phase::Output);
}

void EditExportPage::onDoneClicked() {
    emit backRequested();
}

void EditExportPage::onOpenFolderClicked() {
    const QString folder = QString::fromStdWString(export_output_path_.parent_path().wstring());
    if (!folder.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void EditExportPage::onRevealFileClicked() {
    const QString path = QString::fromStdWString(export_output_path_.wstring());
    if (!path.isEmpty()) {
        // On Windows, use "explorer /select,<path>" to highlight the file in Explorer.
        QProcess::startDetached(QStringLiteral("explorer"),
                                {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
    }
}

void EditExportPage::onRetryExportClicked() {
    runExport();
}

// ---- New slots ----

void EditExportPage::onTrimClicked() {
    // Simple trim dialog: two spin boxes for start/end seconds.
    // Cut points snap to the nearest keyframe (keyframe-accurate trim).
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Set trim points"));
    auto* layout = new QVBoxLayout(&dlg);

    constexpr double kNoTrim = 0.0;
    const double current_start =
        trim_start_us_ != recorder_core::TrimRange::kNoTimestamp ? trim_start_us_ / 1e6 : kNoTrim;
    const double current_end = trim_end_us_ != recorder_core::TrimRange::kNoTimestamp ? trim_end_us_ / 1e6 : kNoTrim;

    auto* start_spin = new QDoubleSpinBox(&dlg);
    start_spin->setPrefix(QStringLiteral("Start:\xc2\xa0"));
    start_spin->setSuffix(QStringLiteral("\xc2\xa0s"));
    start_spin->setDecimals(2);
    start_spin->setMinimum(0.0);
    start_spin->setMaximum(1e6);
    start_spin->setValue(current_start);

    auto* end_spin = new QDoubleSpinBox(&dlg);
    end_spin->setPrefix(QStringLiteral("End:\xc2\xa0"));
    end_spin->setSuffix(QStringLiteral("\xc2\xa0s  (0 = no end trim)"));
    end_spin->setDecimals(2);
    end_spin->setMinimum(0.0);
    end_spin->setMaximum(1e6);
    end_spin->setValue(current_end);

    auto* note =
        new QLabel(QStringLiteral("Trim is keyframe-accurate: cut points snap to the nearest keyframe."), &dlg);
    note->setWordWrap(true);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(start_spin);
    layout->addWidget(end_spin);
    layout->addWidget(note);
    layout->addWidget(btns);

    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const double start_s = start_spin->value();
    const double end_s = end_spin->value();

    // Snap to nearest keyframe at or before the requested time.
    auto snapToKeyframe = [&](double secs) -> int64_t {
        const int64_t us = static_cast<int64_t>(secs * 1e6);
        if (keyframe_timestamps_.empty())
            return us;
        auto it = std::upper_bound(keyframe_timestamps_.begin(), keyframe_timestamps_.end(), us);
        if (it != keyframe_timestamps_.begin())
            --it;
        return *it;
    };

    // Snap to the nearest marker if within 50 ms.
    auto snapToMarker = [&](int64_t us) -> int64_t {
        for (const auto& m : markers_) {
            const int64_t m_us = static_cast<int64_t>(m.time_ms) * 1000LL;
            if (std::abs(m_us - us) <= 50000LL)
                return m_us;
        }
        return us;
    };

    trim_start_us_ = (start_s > 0.0) ? snapToMarker(snapToKeyframe(start_s)) : recorder_core::TrimRange::kNoTimestamp;
    trim_end_us_ = (end_s > 0.0) ? snapToMarker(snapToKeyframe(end_s)) : recorder_core::TrimRange::kNoTimestamp;

    // Update timeline labels.
    auto formatUs = [](int64_t us) -> QString {
        if (us == recorder_core::TrimRange::kNoTimestamp)
            return QStringLiteral("\xe2\x80\x93");
        const int64_t secs = us / 1000000LL;
        const int m2 = static_cast<int>(secs / 60);
        const int s2 = static_cast<int>(secs % 60);
        return QStringLiteral("%1:%2").arg(m2).arg(s2, 2, 10, QLatin1Char('0'));
    };
    if (timeline_in_label_)
        timeline_in_label_->setText(QStringLiteral("In %1").arg(formatUs(trim_start_us_)));
    if (timeline_out_label_)
        timeline_out_label_->setText(QStringLiteral("Out %1").arg(formatUs(trim_end_us_)));
}

void EditExportPage::onAddMarkerClicked() {
    // Without a live playhead, add a marker at the trim-start position or 0.
    const uint64_t time_ms = trim_start_us_ != recorder_core::TrimRange::kNoTimestamp
                                 ? static_cast<uint64_t>(trim_start_us_ / 1000LL)
                                 : 0ULL;
    RecordingMarker m;
    m.time_ms = time_ms;
    m.type = RecordingMarkerType::General;
    m.label = "Marker";
    markers_.push_back(m);
    saveMarkers();
}

// ---- Marker sidecar I/O ----

void EditExportPage::loadMarkers() {
    markers_.clear();
    if (!ctx_.marker_sidecar_path.isEmpty()) {
        QFile f(ctx_.marker_sidecar_path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            const QJsonArray arr = doc.object().value(QStringLiteral("markers")).toArray();
            for (const auto& v : arr) {
                const QJsonObject obj = v.toObject();
                RecordingMarker marker;
                marker.time_ms = static_cast<uint64_t>(obj.value(QStringLiteral("timeMs")).toDouble());
                const QString type_str = obj.value(QStringLiteral("type")).toString();
                if (type_str == QStringLiteral("cut"))
                    marker.type = RecordingMarkerType::Cut;
                else if (type_str == QStringLiteral("highlight"))
                    marker.type = RecordingMarkerType::Highlight;
                else
                    marker.type = RecordingMarkerType::General;
                marker.label = obj.value(QStringLiteral("label")).toString().toStdString();
                markers_.push_back(marker);
            }
            return;
        }
    }
    // Fallback: use markers pre-loaded from the recording session.
    markers_ = ctx_.markers;
}

void EditExportPage::saveMarkers() {
    if (ctx_.marker_sidecar_path.isEmpty())
        return;
    QJsonArray arr;
    for (const auto& m : markers_) {
        QJsonObject obj;
        obj[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
        obj[QStringLiteral("type")] = QString::fromLatin1(RecordingMarkerTypeToString(m.type));
        obj[QStringLiteral("label")] = QString::fromStdString(m.label);
        arr.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("timebase")] = QStringLiteral("milliseconds");
    root[QStringLiteral("markers")] = arr;
    QSaveFile file(ctx_.marker_sidecar_path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

// ---- Real stream-copy export ----

void EditExportPage::runExport() {
    setPhase(Phase::Exporting);

    const QString container_key =
        output_container_combo_ ? output_container_combo_->currentData().toString() : QStringLiteral("mkv");
    const bool overwrite =
        output_save_mode_combo_ && output_save_mode_combo_->currentData().toString() == QStringLiteral("overwrite");
    const bool to_mp4 = (container_key == QStringLiteral("mp4"));

    if (ctx_.mkv_master_path.isEmpty()) {
        setPhase(Phase::Failed);
        if (result_detail_label_)
            result_detail_label_->setText(QStringLiteral("No edit master available for export."));
        return;
    }

    const std::filesystem::path master(ctx_.mkv_master_path.toStdWString());

    // Derive the output path.
    std::filesystem::path output_path;
    if (overwrite) {
        output_path = std::filesystem::path(ctx_.output_path.toStdWString());
    } else {
        std::filesystem::path base(ctx_.output_path.toStdWString());
        const std::wstring ext = to_mp4 ? L".mp4" : L".mkv";
        output_path = base.parent_path() / (base.stem().wstring() + L"_edit" + ext);
    }

    recorder_core::TrimRange tr;
    tr.start_us = trim_start_us_;
    tr.end_us = trim_end_us_;

    export_output_path_ = output_path;

    if (export_thread_.joinable())
        export_thread_.join();
    export_cancel_.store(false);

    export_thread_ = std::thread([this, master, output_path, to_mp4, tr, overwrite]() {
        std::filesystem::path temp_output = output_path;
        temp_output += L".tmp";

        auto progress_cb = [this](float fraction) -> bool {
            if (export_cancel_.load())
                return false;
            QMetaObject::invokeMethod(
                this,
                [this, fraction]() {
                    if (exporting_bar_)
                        exporting_bar_->setValue(static_cast<int>(fraction * 100.0f));
                },
                Qt::QueuedConnection);
            return true;
        };

        recorder_core::RemuxResult res;
        if (to_mp4)
            res = recorder_core::RemuxToProgressiveMp4(master, temp_output, progress_cb, tr);
        else
            res = recorder_core::RemuxToMkv(master, temp_output, progress_cb, tr);

        bool ok = res.success;
        std::string err_msg = res.message;

        if (ok) {
            // Atomic replace: rename temp → final (same volume = atomic on Windows NTFS).
            std::error_code ec;
            std::filesystem::rename(temp_output, output_path, ec);
            if (ec) {
                ok = false;
                err_msg = "Failed to save output file: " + ec.message();
                std::error_code del_ec;
                std::filesystem::remove(temp_output, del_ec);
            }
        } else {
            // Clean up failed / cancelled temp file.
            std::error_code del_ec;
            std::filesystem::remove(temp_output, del_ec);
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, err_msg, output_path]() {
                export_output_path_ = output_path;
                if (ok) {
                    setPhase(Phase::Done);
                    if (result_detail_label_) {
                        result_detail_label_->setText(QString::fromStdWString(output_path.filename().wstring()) +
                                                      QStringLiteral(" \xc2\xb7 stream-copy \xc2\xb7 lossless"));
                    }
                    emit exportCompleted(QString::fromStdWString(output_path.wstring()));
                } else {
                    setPhase(Phase::Failed);
                    if (result_detail_label_)
                        result_detail_label_->setText(QString::fromStdString(err_msg));
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace exosnap
