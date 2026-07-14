#include "EditExportPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"
#include "../ui/widgets/EditPlayerSurface.h"
#include "../ui/widgets/EditTimeline.h"

#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRectF>
#include <QScrollArea>
#include <QSize>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>

#include "../models/EditTimelineModel.h"
#include "../models/MarkerSidecar.h"

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
    if (key == QLatin1String("pause"))
        return QByteArrayLiteral("M9 5v14M15 5v14");
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

// Preview playback clock granularity (~30 fps playhead updates).
constexpr int kPreviewTickMs = 33;

} // namespace

EditExportPage::EditExportPage(QWidget* parent) : QWidget(parent) {
    preview_timer_ = new QTimer(this);
    preview_timer_->setInterval(kPreviewTickMs);
    connect(preview_timer_, &QTimer::timeout, this, &EditExportPage::onPreviewTick);
    preview_elapsed_ = new QElapsedTimer();
    buildUi();
}

EditExportPage::~EditExportPage() {
    export_cancel_.store(true);
    if (export_thread_.joinable())
        export_thread_.join();
    delete preview_elapsed_;
}

void EditExportPage::buildUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ---- Mode-Bar ----
    mode_bar_ = new QFrame(this);
    mode_bar_->setObjectName(QStringLiteral("editExportModeBar"));
    mode_bar_->setFixedHeight(52);

    auto* mode_bar_layout = new QHBoxLayout(mode_bar_);
    mode_bar_layout->setContentsMargins(M::kSpaceMd, 0, M::kSpaceMd, 0);
    mode_bar_layout->setSpacing(M::kSpaceSm);

    back_btn_ = new QPushButton(mode_bar_);
    back_btn_->setObjectName(QStringLiteral("editExportBackBtn"));
    back_btn_->setFixedSize(32, 32);
    back_btn_->setToolTip(QStringLiteral("Back to Record"));
    back_btn_->setCursor(Qt::PointingHandCursor);
    back_btn_->setIconSize(QSize(16, 16));

    title_label_ = new QLabel(QStringLiteral("Edit & export"), mode_bar_);
    title_label_->setObjectName(QStringLiteral("editExportTitle"));

    filename_label_ = new QLabel(this);
    filename_label_->setObjectName(QStringLiteral("editExportFilename"));
    filename_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    mode_bar_layout->addWidget(back_btn_);
    mode_bar_layout->addWidget(title_label_);
    mode_bar_layout->addWidget(filename_label_, 1);
    mode_bar_layout->addStretch();

    root_layout->addWidget(mode_bar_);

    // ---- Phase Stepper ----
    stepper_widget_ = new QWidget(this);
    stepper_widget_->setObjectName(QStringLiteral("editExportStepper"));
    stepper_widget_->setFixedHeight(40);

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

    // ---- Left pane (player + timeline + output + exporting + result panels) ----
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

    auto* player_layout = new QVBoxLayout(player_frame_);
    player_layout->setAlignment(Qt::AlignCenter);

    // 60px circular play/pause toggle. Drives the preview position clock and
    // the real decoder session (player_session_).
    play_pause_btn_ = new QPushButton(player_frame_);
    play_pause_btn_->setObjectName(QStringLiteral("editExportPlayPauseBtn"));
    play_pause_btn_->setFixedSize(60, 60);
    play_pause_btn_->setCursor(Qt::PointingHandCursor);
    play_pause_btn_->setIconSize(QSize(24, 24));
    play_pause_btn_->setToolTip(QStringLiteral("Play / pause preview"));

    player_surface_ = new ui::widgets::EditPlayerSurface(player_frame_);

    player_meta_label_ = new QLabel(this);
    player_meta_label_->setObjectName(QStringLiteral("editExportPlayerMeta"));
    player_meta_label_->setAlignment(Qt::AlignRight | Qt::AlignBottom);

    player_layout->addStretch();
    player_layout->addWidget(play_pause_btn_, 0, Qt::AlignHCenter);
    player_layout->addWidget(player_surface_);
    player_layout->addStretch();
    player_layout->addWidget(player_meta_label_);

    left_layout->addWidget(player_frame_);

    // Review Panel (post-flight report, shown only in Review phase)
    review_panel_ = new QWidget(left_widget);
    review_panel_->setObjectName(QStringLiteral("editExportReviewPanel"));
    auto* review_layout = new QVBoxLayout(review_panel_);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->setSpacing(M::kSpaceSm);

    review_title_ = new QLabel(QStringLiteral("Post-recording report"), review_panel_);

    review_drop_label_ = new QLabel(QStringLiteral("Frame drops: \xe2\x80\x93"), review_panel_);

    review_drift_label_ = new QLabel(QStringLiteral("Peak A/V drift: \xe2\x80\x93"), review_panel_);

    review_health_label_ = new QLabel(QStringLiteral("Pipeline health: \xe2\x80\x93"), review_panel_);

    review_layout->addWidget(review_title_);
    review_layout->addWidget(review_drop_label_);
    review_layout->addWidget(review_drift_label_);
    review_layout->addWidget(review_health_label_);
    left_layout->addWidget(review_panel_);

    // Timeline: trim handles, markers, and the playhead live directly on the
    // strip — there is no button row or duration readout above it.
    timeline_ = new ui::widgets::EditTimeline(left_widget);
    timeline_->setObjectName(QStringLiteral("editTimeline"));
    left_layout->addWidget(timeline_);

    connect(timeline_, &ui::widgets::EditTimeline::trimHandleReleased, this, &EditExportPage::onTrimHandleReleased);
    connect(timeline_, &ui::widgets::EditTimeline::scrubStarted, this, &EditExportPage::onScrubStarted);
    connect(timeline_, &ui::widgets::EditTimeline::scrubMoved, this, &EditExportPage::onScrubMoved);
    connect(timeline_, &ui::widgets::EditTimeline::scrubFinished, this, &EditExportPage::onScrubFinished);

    // Output Panel (container + save-mode selectors)
    output_panel_ = new QWidget(left_widget);
    output_panel_->setObjectName(QStringLiteral("editExportOutputPanel"));
    auto* output_panel_layout = new QVBoxLayout(output_panel_);
    output_panel_layout->setContentsMargins(0, 0, 0, 0);
    output_panel_layout->setSpacing(M::kSpaceSm);

    output_title_ = new QLabel(QStringLiteral("Output format"), output_panel_);
    output_panel_layout->addWidget(output_title_);

    // Container selection (stream-copy only — no re-encode per ADR-0014)
    container_lbl_ = new QLabel(QStringLiteral("Container:"), output_panel_);
    output_panel_layout->addWidget(container_lbl_);

    output_container_combo_ = new QComboBox(output_panel_);
    output_container_combo_->setObjectName(QStringLiteral("outputContainerCombo"));
    output_container_combo_->addItem(QStringLiteral("MKV  \xe2\x80\x93  stream-copy, lossless"), QStringLiteral("mkv"));
    output_container_combo_->addItem(QStringLiteral("MP4  \xe2\x80\x93  stream-copy, lossless (ADR\xc2\xa0"
                                                    "0014)"),
                                     QStringLiteral("mp4"));
    output_panel_layout->addWidget(output_container_combo_);

    // Save mode: new file or overwrite original
    savemode_lbl_ = new QLabel(QStringLiteral("Save:"), output_panel_);
    output_panel_layout->addWidget(savemode_lbl_);

    output_save_mode_combo_ = new QComboBox(output_panel_);
    output_save_mode_combo_->setObjectName(QStringLiteral("outputSaveModeCombo"));
    output_save_mode_combo_->addItem(QStringLiteral("Save as new file  (\xe2\x80\x9c<name>_edit.<ext>\xe2\x80\x9d)"),
                                     QStringLiteral("new"));
    output_save_mode_combo_->addItem(QStringLiteral("Overwrite original  (atomic replace)"),
                                     QStringLiteral("overwrite"));
    output_panel_layout->addWidget(output_save_mode_combo_);

    // Destination row. The save mode above fully determines the destination
    // (new file = beside the source; overwrite = the source's own location) —
    // there is no user-choosable destination folder in this model, so this row
    // is informational only (no Browse button; see ADR 0022).
    auto* dest_row = new QWidget(output_panel_);
    auto* dest_layout = new QHBoxLayout(dest_row);
    dest_layout->setContentsMargins(0, 0, 0, 0);
    dest_layout->setSpacing(M::kSpaceSm);

    dest_lbl_title_ = new QLabel(QStringLiteral("Destination:"), dest_row);

    dest_folder_label_ = new QLabel(QStringLiteral("Same folder as source"), dest_row);
    dest_folder_label_->setObjectName(QStringLiteral("editExportDestFolder"));

    dest_layout->addWidget(dest_lbl_title_);
    dest_layout->addWidget(dest_folder_label_, 1);

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

    exporting_bar_ = new QProgressBar(exporting_panel_);
    exporting_bar_->setObjectName(QStringLiteral("editExportProgressBar"));
    exporting_bar_->setRange(0, 100);
    exporting_bar_->setValue(62);
    exporting_bar_->setFixedHeight(6);
    exporting_bar_->setTextVisible(false);

    exporting_detail_label_ = new QLabel(QStringLiteral("Stream-copy \xc2\xb7 no quality loss"), exporting_panel_);

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

    // ---- Right pane: Details card (right-aligned mono values) ----
    auto* rail_column = new QWidget(content_area);
    rail_column->setFixedWidth(280);
    auto* rail_column_layout = new QVBoxLayout(rail_column);
    rail_column_layout->setContentsMargins(M::kSpaceSm, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    rail_column_layout->setSpacing(0);

    detail_rail_ = new QFrame(rail_column);
    detail_rail_->setObjectName(QStringLiteral("editExportDetailRail"));

    auto* rail_layout = new QVBoxLayout(detail_rail_);
    rail_layout->setContentsMargins(M::kSpaceMd, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    rail_layout->setSpacing(0);

    rail_title_ = new QLabel(QStringLiteral("Details"), detail_rail_);
    rail_layout->addWidget(rail_title_);
    rail_layout->addSpacing(M::kSpaceSm);

    const auto makeFactRow = [&](const QString& key_text, QLabel*& val_label_ref, bool first) {
        if (!first) {
            auto* sep = new QFrame(detail_rail_);
            sep->setFixedHeight(1);
            fact_separators_.push_back(sep);
            rail_layout->addWidget(sep);
        }
        auto* row = new QWidget(detail_rail_);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 7, 0, 7);
        row_layout->setSpacing(M::kSpaceSm);

        auto* key = new QLabel(key_text, row);
        fact_keys_.push_back(key);

        val_label_ref = new QLabel(QStringLiteral("–"), row);
        val_label_ref->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        row_layout->addWidget(key);
        row_layout->addWidget(val_label_ref, 1);
        rail_layout->addWidget(row);
    };

    makeFactRow(QStringLiteral("Duration"), fact_duration_val_, true);
    fact_duration_val_->setObjectName(QStringLiteral("editFactDuration"));

    makeFactRow(QStringLiteral("Size"), fact_size_val_, false);
    fact_size_val_->setObjectName(QStringLiteral("editFactSize"));

    makeFactRow(QStringLiteral("Resolution"), fact_res_val_, false);
    fact_res_val_->setObjectName(QStringLiteral("editFactResolution"));

    makeFactRow(QStringLiteral("Frame rate"), fact_fps_val_, false);
    fact_fps_val_->setObjectName(QStringLiteral("editFactFps"));

    makeFactRow(QStringLiteral("Video"), fact_video_val_, false);
    fact_video_val_->setObjectName(QStringLiteral("editFactVideo"));

    makeFactRow(QStringLiteral("Audio"), fact_audio_val_, false);
    fact_audio_val_->setObjectName(QStringLiteral("editFactAudio"));

    makeFactRow(QStringLiteral("Container"), fact_container_val_, false);
    fact_container_val_->setObjectName(QStringLiteral("editFactContainer"));

    rail_column_layout->addWidget(detail_rail_);
    rail_column_layout->addStretch();

    content_layout->addWidget(left_scroll, 1);
    content_layout->addWidget(rail_column);

    root_layout->addWidget(content_area, 1);

    // ---- Bottom action bar: the Save action sits bottom-right, in the same
    // position the Record page keeps its primary transport actions. ----
    action_bar_ = new QFrame(this);
    action_bar_->setObjectName(QStringLiteral("editExportActionBar"));
    action_bar_->setFixedHeight(64);

    auto* action_layout = new QHBoxLayout(action_bar_);
    action_layout->setContentsMargins(M::kSpaceMd, 0, M::kSpaceMd, 0);
    action_layout->setSpacing(M::kSpaceSm);
    action_layout->addStretch();

    secondary_action_btn_ = new QPushButton(action_bar_);
    secondary_action_btn_->setObjectName(QStringLiteral("editExportSecondaryBtn"));
    secondary_action_btn_->setProperty("role", "ghost");
    secondary_action_btn_->hide();

    // "&&" renders as a literal ampersand (a single "&" would become a mnemonic).
    primary_action_btn_ = new QPushButton(QStringLiteral("Save && export"), action_bar_);
    primary_action_btn_->setObjectName(QStringLiteral("editExportPrimaryBtn"));
    primary_action_btn_->setProperty("role", "primary");
    primary_action_btn_->setMinimumWidth(150);

    action_layout->addWidget(secondary_action_btn_);
    action_layout->addWidget(primary_action_btn_);

    root_layout->addWidget(action_bar_);

    // Wire signals
    connect(back_btn_, &QPushButton::clicked, this, &EditExportPage::onBackClicked);
    connect(play_pause_btn_, &QPushButton::clicked, this, [this]() { setPreviewPlaying(!preview_playing_); });
    connect(primary_action_btn_, &QPushButton::clicked, this, [this]() {
        switch (phase_) {
        case Phase::Review:
            setPhase(Phase::Edit);
            break;
        case Phase::Edit:
            setPhase(Phase::Output);
            break;
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
        }
    });
    connect(secondary_action_btn_, &QPushButton::clicked, this, &EditExportPage::onOpenFolderClicked);
    connect(result_open_folder_btn_, &QPushButton::clicked, this, &EditExportPage::onOpenFolderClicked);
    connect(result_reveal_btn_, &QPushButton::clicked, this, &EditExportPage::onRevealFileClicked);

    // Applies the theme-derived inline styling (and the initial phase via
    // refreshPhase()) now, and re-applies it on every theme switch so nothing
    // keeps the old palette's colours or icon tints.
    ui::theme::OnThemeChanged(this, [this]() { applyThemeStyles(); });
}

void EditExportPage::applyThemeStyles() {
    const auto& t = ActiveTheme();

    // ---- Mode-Bar ----
    mode_bar_->setStyleSheet(QStringLiteral("QFrame#editExportModeBar {"
                                            "background:%1;"
                                            "border-bottom: 1px solid %2;"
                                            "}")
                                 .arg(t.surf, t.line));

    back_btn_->setIcon(QIcon(renderEditIcon(QStringLiteral("chevLeft"), 16, themeColor(t.mut))));
    back_btn_->setStyleSheet(QStringLiteral("QPushButton#editExportBackBtn {"
                                            "background:%1;"
                                            "border: 1px solid %2;"
                                            "border-radius: 9px;"
                                            "}"
                                            "QPushButton#editExportBackBtn:hover { background:%3; }")
                                 .arg(t.surf2, t.line2, t.raise));

    title_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-weight:700; font-size:16px; }").arg(t.ink));

    filename_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-family:'IBM Plex Mono','Consolas',monospace; font-size:12.5px; }")
            .arg(t.ac));

    // ---- Phase Stepper ----
    stepper_widget_->setStyleSheet(QStringLiteral("QWidget#editExportStepper {"
                                                  "background:%1;"
                                                  "border-bottom: 1px solid %2;"
                                                  "}")
                                       .arg(t.surf, t.line));

    // ---- Player-Area ----
    player_frame_->setStyleSheet(QStringLiteral("QFrame#editExportPlayer {"
                                                "background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
                                                "stop:0 #1a1a1e, stop:1 #0e0e10);"
                                                "border: 1px solid %1;"
                                                "border-radius: %2px;"
                                                "}")
                                     .arg(t.line)
                                     .arg(M::kRadiusLg));

    play_pause_btn_->setStyleSheet(QStringLiteral("QPushButton#editExportPlayPauseBtn {"
                                                  "background: rgba(14, 14, 16, 0.7);"
                                                  "border: 1px solid %1;"
                                                  "border-radius: 30px;"
                                                  "}"
                                                  "QPushButton#editExportPlayPauseBtn:hover {"
                                                  "background: rgba(24, 24, 28, 0.8);"
                                                  "}")
                                       .arg(t.line2));
    // Re-render the play/pause glyph (its tint follows ActiveTheme().ink).
    refreshPlayButton();

    // (player_surface_ paints its own panel/placeholder; no QSS involvement.)
    player_meta_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:10px; }").arg(t.dim));

    // ---- Review Panel ----
    review_title_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(t.ink));
    review_drop_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(t.mut));
    review_drift_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(t.mut));
    review_health_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(t.mut));

    // ---- Output Panel ----
    output_title_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(t.ink));
    container_lbl_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(t.mut));
    savemode_lbl_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(t.mut));
    dest_lbl_title_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(t.mut));
    dest_folder_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(ThemeText1Color(t)));

    // ---- Exporting Panel ----
    exporting_status_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:14px; }").arg(t.ink));
    exporting_bar_->setStyleSheet(QStringLiteral("QProgressBar { background:%1; border-radius:3px; border:none; }"
                                                 "QProgressBar::chunk { background:%2; border-radius:3px; }")
                                      .arg(t.raise, t.ac));
    exporting_detail_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(t.mut));

    // ---- Result Panel (title/icon/badge are phase-dependent → refreshPhase) ----
    result_detail_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(t.mut));

    // ---- Details card (right rail) ----
    detail_rail_->setStyleSheet(QStringLiteral("QFrame#editExportDetailRail {"
                                               "background:%1;"
                                               "border: 1px solid %2;"
                                               "border-radius: %3px;"
                                               "}")
                                    .arg(t.surf, t.line)
                                    .arg(M::kRadiusLg));
    rail_title_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-weight:700; font-size:13.5px; }").arg(t.ink));

    for (QFrame* sep : fact_separators_)
        sep->setStyleSheet(QStringLiteral("QFrame { background:%1; border:none; }").arg(t.line));
    for (QLabel* key : fact_keys_)
        key->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-family:'IBM Plex Mono','Consolas',monospace; font-size:11px; }")
                .arg(t.dim));
    for (QLabel* val : {fact_duration_val_, fact_size_val_, fact_res_val_, fact_fps_val_, fact_video_val_,
                        fact_audio_val_, fact_container_val_})
        val->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-family:'IBM Plex Mono','Consolas',monospace; font-size:12px; }")
                .arg(t.ink));

    // ---- Bottom action bar ----
    action_bar_->setStyleSheet(QStringLiteral("QFrame#editExportActionBar {"
                                              "background:%1;"
                                              "border-top: 1px solid %2;"
                                              "}")
                                   .arg(t.surf, t.line));

    // Re-derive the phase-dependent stepper/result/title/button styling and icons
    // for the current phase from ActiveTheme().
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

    // --- Open the real decoder session for the new clip (replaces the previous one, if any) ---
    // Clear any previous clip's frame first: until the new session delivers a
    // frame the surface shows its placeholder, which doubles as the
    // "Preview unavailable" fallback when Open() fails.
    if (player_surface_)
        player_surface_->clearFrame();
    player_session_ = std::make_unique<recorder_core::EditPlayerSession>();
    if (!ctx_.mkv_master_path.isEmpty()) {
        std::string open_err;
        const bool opened = player_session_->Open(std::filesystem::path(ctx_.mkv_master_path.toStdWString()), open_err);
        if (opened) {
            player_session_->SetOnFrameReady([this](recorder_core::DecodedVideoFrame frame) {
                // Invoked from the session's internal seek-worker thread
                // (scrub/trim-drag path only -- continuous playback frames
                // now go through PollFrame() in onPreviewTick() instead, see
                // below). Never touch player_surface_ here; marshal onto the
                // UI thread.
                QMetaObject::invokeMethod(this, "onDecodedFrameReady", Qt::QueuedConnection,
                                          Q_ARG(QImage, DecodedFrameToQImage(frame)));
            });
            // Show the clip's first frame as a poster instead of the
            // placeholder while the user is still reviewing.
            player_session_->SeekTo(0);
        }
    }

    // --- Reset the preview clock and the timeline for the new clip ---
    duration_seconds_ = ctx_.duration_seconds;
    setPreviewPlaying(false);
    preview_position_ms_ = 0;
    if (timeline_)
        timeline_->setDurationMs(durationMs());

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
}

void EditExportPage::setPhase(Phase phase) {
    phase_ = phase;
    refreshPhase();
}

// ---- Preview playback clock ----

qint64 EditExportPage::durationMs() const noexcept {
    return duration_seconds_ > 0.0 ? static_cast<qint64>(std::llround(duration_seconds_ * 1000.0)) : 0;
}

void EditExportPage::setPreviewPlaying(bool playing) {
    if (playing == preview_playing_)
        return;
    if (playing && durationMs() <= 0)
        return; // unknown duration: nothing to play against
    preview_playing_ = playing;
    if (preview_playing_) {
        preview_elapsed_->restart();
        preview_timer_->start();
        // Continuous decode (EditPlayerSession::Play()) is only engaged when
        // there's an audio stream to pace it against -- see onPreviewTick().
        // A clip with no audio stream is driven entirely by the per-tick
        // SeekTo() fallback there instead; starting continuous decode here
        // for a no-audio clip would just race through the file unthrottled
        // for frames nothing ever consumes (see
        // docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md).
        if (player_session_ && player_session_->HasAudioStream())
            player_session_->Play(preview_position_ms_ * 1000); // ms -> us: resume from where the
                                                                // playhead actually is (a pause or
                                                                // a prior scrub), not the beginning
    } else {
        preview_timer_->stop();
        if (player_session_)
            player_session_->Pause();
    }
    refreshPlayButton();
}

void EditExportPage::setPreviewPositionMs(qint64 position_ms) {
    preview_position_ms_ = ClampPlayheadMs(position_ms, durationMs());
    if (timeline_)
        timeline_->setPositionMs(preview_position_ms_);
}

void EditExportPage::setTrimRangeMs(qint64 start_ms, qint64 end_ms) {
    if (!timeline_ || durationMs() <= 0)
        return;
    timeline_->setTrimRangeMs(start_ms, end_ms);
    trim_start_us_ =
        timeline_->trimStartMs() > 0 ? timeline_->trimStartMs() * 1000 : recorder_core::TrimRange::kNoTimestamp;
    trim_end_us_ =
        timeline_->trimEndMs() < durationMs() ? timeline_->trimEndMs() * 1000 : recorder_core::TrimRange::kNoTimestamp;
}

void EditExportPage::refreshPlayButton() {
    if (!play_pause_btn_)
        return;
    const QString glyph = preview_playing_ ? QStringLiteral("pause") : QStringLiteral("play");
    play_pause_btn_->setIcon(QIcon(renderEditIcon(glyph, 24, themeColor(ActiveTheme().ink))));
}

QImage EditExportPage::DecodedFrameToQImage(const recorder_core::DecodedVideoFrame& frame) {
    const QImage img(frame.bgra->data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                     static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32);
    return img.copy(); // detach: frame.bgra's buffer lifetime is not guaranteed beyond this call
}

void EditExportPage::onPreviewTick() {
    const bool paced_by_audio = player_session_ && player_session_->HasAudioStream();
    if (paced_by_audio) {
        // Audio is the pacing AND position source of truth while it exists
        // -- no independent wall-clock estimate to keep in sync with it.
        preview_position_ms_ = ClampPlayheadMs(player_session_->CurrentPositionMs(), durationMs());
        if (auto frame = player_session_->PollFrame())
            onDecodedFrameReady(DecodedFrameToQImage(*frame)); // already on the UI thread: direct call
    } else {
        preview_position_ms_ += preview_elapsed_->restart();
        if (player_session_)
            player_session_->SeekTo(preview_position_ms_ * 1000); // ms -> us: no-audio pacing fallback
                                                                  // (safe no-op if not open, matching
                                                                  // EditPlayerSession's own contract)
    }

    const qint64 total = durationMs();
    if (preview_position_ms_ >= total) {
        preview_position_ms_ = total;
        setPreviewPlaying(false); // reached the end: pause there
    }
    if (timeline_)
        timeline_->setPositionMs(preview_position_ms_);
}

void EditExportPage::onDecodedFrameReady(QImage frame) {
    if (player_surface_)
        player_surface_->setFrame(std::move(frame));
}

// ---- Timeline interaction ----

void EditExportPage::onTrimHandleReleased(qint64 start_ms, qint64 end_ms) {
    // Snap to the nearest keyframe at or before the requested time (keyframe-
    // accurate trim), then to the nearest marker within 50 ms.
    const auto snapToKeyframe = [&](int64_t us) -> int64_t {
        if (keyframe_timestamps_.empty())
            return us;
        auto it = std::upper_bound(keyframe_timestamps_.begin(), keyframe_timestamps_.end(), us);
        if (it != keyframe_timestamps_.begin())
            --it;
        return *it;
    };
    const auto snapToMarker = [&](int64_t us) -> int64_t {
        for (const auto& m : markers_) {
            const int64_t m_us = static_cast<int64_t>(m.time_ms) * 1000LL;
            if (std::abs(m_us - us) <= 50000LL)
                return m_us;
        }
        return us;
    };

    const qint64 total_ms = durationMs();

    if (start_ms <= 0) {
        trim_start_us_ = recorder_core::TrimRange::kNoTimestamp;
    } else {
        trim_start_us_ = snapToMarker(snapToKeyframe(start_ms * 1000));
    }
    if (end_ms >= total_ms) {
        trim_end_us_ = recorder_core::TrimRange::kNoTimestamp;
    } else {
        trim_end_us_ = snapToMarker(snapToKeyframe(end_ms * 1000));
    }

    // Write the snapped values back so the handles land where the cut will be.
    if (timeline_) {
        const qint64 snapped_start =
            trim_start_us_ != recorder_core::TrimRange::kNoTimestamp ? trim_start_us_ / 1000 : 0;
        const qint64 snapped_end =
            trim_end_us_ != recorder_core::TrimRange::kNoTimestamp ? trim_end_us_ / 1000 : total_ms;
        timeline_->setTrimRangeMs(snapped_start, snapped_end);
    }

    // Show the frame at the (possibly snapped) boundary the handle landed on.
    if (player_session_) {
        const int64_t shown_us = (start_ms <= 0) ? trim_end_us_ : trim_start_us_;
        if (shown_us != recorder_core::TrimRange::kNoTimestamp)
            player_session_->SeekTo(shown_us);
    }
}

void EditExportPage::onScrubStarted() {
    // Scrubbing pauses; whether it resumes on release depends on whether the
    // preview was playing when the scrub began.
    resume_after_scrub_ = preview_playing_;
    setPreviewPlaying(false);
}

void EditExportPage::onScrubMoved(qint64 position_ms) {
    // The preview position follows the drag; the decoder shows the frame at
    // the scrub target (a newer SeekTo supersedes an in-flight older one).
    preview_position_ms_ = ClampPlayheadMs(position_ms, durationMs());
    if (player_session_)
        player_session_->SeekTo(preview_position_ms_ * 1000); // ms -> us
}

void EditExportPage::onScrubFinished() {
    if (resume_after_scrub_)
        setPreviewPlaying(true);
    resume_after_scrub_ = false;
}

void EditExportPage::refreshPhase() {
    // ---- Stepper highlight ----
    // Each phase highlights its own step; Exporting/Done/Failed keep "Output" active.
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
    const bool show_timeline = (phase_ == Phase::Edit);
    const bool show_output = (phase_ == Phase::Output);
    const bool show_exporting = (phase_ == Phase::Exporting);
    const bool show_result = (phase_ == Phase::Done || phase_ == Phase::Failed);

    if (review_panel_)
        review_panel_->setVisible(show_review_panel);
    if (player_frame_)
        player_frame_->setVisible(show_player);
    if (timeline_)
        timeline_->setVisible(show_timeline);
    if (output_panel_)
        output_panel_->setVisible(show_output);
    if (exporting_panel_)
        exporting_panel_->setVisible(show_exporting);
    if (result_panel_)
        result_panel_->setVisible(show_result);

    // The preview clock only makes sense while the player is on screen.
    if (!show_player)
        setPreviewPlaying(false);

    // Panel visibility changed: the height budget for the player moved too.
    updatePlayerHeight();

    // Update primary/secondary buttons
    if (!primary_action_btn_ || !secondary_action_btn_)
        return;

    secondary_action_btn_->hide();

    switch (phase_) {
    case Phase::Review:
        primary_action_btn_->setText(QStringLiteral("Continue to edit"));
        primary_action_btn_->setProperty("role", "ghost");
        break;
    case Phase::Edit:
        primary_action_btn_->setText(QStringLiteral("Continue to output"));
        primary_action_btn_->setProperty("role", "ghost");
        break;
    case Phase::Output:
        // "&&" renders as a literal ampersand (a single "&" would become a mnemonic).
        primary_action_btn_->setText(QStringLiteral("Save && export"));
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
        if (result_detail_label_) {
            const QString file_name = !export_output_path_.empty()
                                          ? QString::fromStdWString(export_output_path_.filename().wstring())
                                          : QStringLiteral("Export");
            result_detail_label_->setText(QStringLiteral("%1 \xc2\xb7 stream-copy \xc2\xb7 lossless").arg(file_name));
        }
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
        if (result_detail_label_) {
            const QString reason = last_export_error_.isEmpty() ? QStringLiteral("unknown error") : last_export_error_;
            result_detail_label_->setText(QStringLiteral("Export failed \xe2\x80\x94 %1").arg(reason));
        }
        break;
    }

    // Force style refresh for property-driven QSS
    if (primary_action_btn_) {
        primary_action_btn_->style()->unpolish(primary_action_btn_);
        primary_action_btn_->style()->polish(primary_action_btn_);
    }
}

bool EditExportPage::eventFilter(QObject* obj, QEvent* event) {
    if (obj == player_frame_ && event->type() == QEvent::Resize)
        updatePlayerHeight();
    return QWidget::eventFilter(obj, event);
}

void EditExportPage::updatePlayerHeight() {
    // Aim for 16:9 relative to the player's current width, but cap the height
    // so the content below it (post-recording report / trim timeline) stays
    // reachable without scrolling — a real video view letterboxes inside the
    // frame anyway.
    if (!player_frame_)
        return;
    const int w = player_frame_->width();
    int target = qRound(w * 9.0 / 16.0);
    int reserved = 52 /* mode bar */ + 40 /* stepper */ + 64 /* action bar */ + 2 * M::kSpaceMd;
    if (timeline_ && !timeline_->isHidden())
        reserved += timeline_->height() + M::kSpaceMd;
    if (review_panel_ && !review_panel_->isHidden())
        reserved += review_panel_->sizeHint().height() + M::kSpaceMd;
    const int max_h = std::max(180, height() - reserved);
    target = std::min(target, max_h);
    if (target > 0 && player_frame_->height() != target)
        player_frame_->setFixedHeight(target);
}

void EditExportPage::hideEvent(QHideEvent* event) {
    // Overlay dismissed / page hidden: the preview clock must not keep running.
    setPreviewPlaying(false);
    // ...and neither must the decoder session's worker threads (or its WASAPI
    // renderer). setEditContext() opens a fresh session the next time the
    // overlay is shown for a clip.
    if (player_session_)
        player_session_->Close();
    QWidget::hideEvent(event);
}

// ---- Slots ----

void EditExportPage::onBackClicked() {
    // Three-step flow (ADR 0022): Back steps to the previous phase for Edit and
    // Output. Review is the first step, so its Back keeps closing the overlay.
    switch (phase_) {
    case Phase::Edit:
        setPhase(Phase::Review);
        return;
    case Phase::Output:
        setPhase(Phase::Edit);
        return;
    default:
        break;
    }
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

// ---- Marker sidecar I/O ----

void EditExportPage::loadMarkers() {
    markers_.clear();
    // Canonical source: the existing "<media>.markers.json" sidecar written by
    // RecordingCoordinator (on AddMarker / on stop). Once that file exists it is
    // authoritative — read it via the shared serializer (models/MarkerSidecar.h).
    if (!ctx_.marker_sidecar_path.isEmpty()) {
        const std::filesystem::path sidecar(ctx_.marker_sidecar_path.toStdWString());
        std::error_code ec;
        if (std::filesystem::exists(sidecar, ec)) {
            markers_ = ReadMarkerSidecar(sidecar);
            if (timeline_)
                timeline_->setMarkers(markers_);
            return;
        }
    }
    // No sidecar on disk: fall back to the markers carried in the result.
    markers_ = ctx_.markers;
    if (timeline_)
        timeline_->setMarkers(markers_);
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
        last_export_error_ = QStringLiteral("No edit master available for export.");
        setPhase(Phase::Failed);
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

    // Markers ride along the export as a retimed JSON sidecar — never as
    // container chapters. Plan it now (snapshot markers + trim on the UI
    // thread) so the export thread races nothing.
    const qint64 window_start_ms = trim_start_us_ != recorder_core::TrimRange::kNoTimestamp ? trim_start_us_ / 1000 : 0;
    const qint64 window_end_ms = trim_end_us_ != recorder_core::TrimRange::kNoTimestamp
                                     ? trim_end_us_ / 1000
                                     : std::numeric_limits<qint64>::max();
    MarkerExportPlan marker_plan =
        PlanMarkerSidecarForExport(output_path, RetimeMarkersForTrim(markers_, window_start_ms, window_end_ms));

    export_output_path_ = output_path;

    if (export_thread_.joinable())
        export_thread_.join();
    export_cancel_.store(false);

    export_thread_ = std::thread([this, master, output_path, to_mp4, tr, marker_plan = std::move(marker_plan)]() {
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

        if (ok) {
            // Retimed marker sidecar beside the exported file — written only
            // when markers survived the trim; a stale sidecar at the
            // destination (overwrite-original export) is removed otherwise.
            ApplyMarkerExportPlan(marker_plan);
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, err_msg, output_path]() {
                export_output_path_ = output_path;
                if (ok) {
                    // refreshPhase() derives the Done detail text from
                    // export_output_path_ (set just above) — the real output
                    // filename, not a placeholder.
                    setPhase(Phase::Done);
                    emit exportCompleted(QString::fromStdWString(output_path.wstring()));
                } else {
                    // refreshPhase() derives the Failed detail text from
                    // last_export_error_ (set just below) — the real remuxer
                    // error, not a hardcoded placeholder.
                    last_export_error_ = QString::fromStdString(err_msg);
                    setPhase(Phase::Failed);
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace exosnap
