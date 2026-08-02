#include "EditExportPage.h"

#include "../ui/dialogs/ExportOverlay.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"
#include "../ui/widgets/EditDetailsRail.h"
#include "../ui/widgets/EditPlayerSurface.h"
#include "../ui/widgets/EditTimeline.h"

#include <QAbstractButton>
#include <QByteArray>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRectF>
#include <QScreen>
#include <QScrollArea>
#include <QSize>
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
    if (key == QLatin1String("info"))
        return QByteArrayLiteral("M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20zM12 16v-4M12 8h.01");
    if (key == QLatin1String("alertTriangle"))
        return QByteArrayLiteral("M21.73 18l-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3z"
                                 "M12 9v4M12 17h.01");
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

// Fallback preview tick, used only until a clip is open (or when its
// container declares no usable frame rate). The real interval comes from the
// clip itself -- see EditExportPage::refreshPreviewTickInterval().
constexpr int kFallbackPreviewTickMs = 16;

// Presentation cadence for a clip at `clip_fps`, capped at `screen_hz`.
// Painting more often than the display refreshes cannot show more motion, it
// only burns a full-frame convert+blit per wasted tick -- so a 144 fps clip
// runs at 144 on a 144 Hz panel and at 60 on a 60 Hz one.
int PreviewTickMsFor(double clip_fps, double screen_hz) noexcept {
    double hz = clip_fps;
    if (!(hz > 0.0))
        return kFallbackPreviewTickMs;
    if (screen_hz > 0.0)
        hz = std::min(hz, screen_hz);
    // Round down so the timer never lands just past a frame boundary, and
    // never go below 1 ms (a QTimer cannot honour 0 as "once per frame").
    return std::max(1, static_cast<int>(std::floor(1000.0 / hz)));
}

} // namespace

EditExportPage::EditExportPage(QWidget* parent) : QWidget(parent) {
    preview_timer_ = new QTimer(this);
    // Coarse timers are allowed a 5% slop, which Qt happily spends at these
    // intervals -- at 144 fps (6 ms) that is most of a frame.
    preview_timer_->setTimerType(Qt::PreciseTimer);
    preview_timer_->setInterval(kFallbackPreviewTickMs);
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

    // Post-flight report: quiet by default (an info glyph whose tooltip carries
    // the three numbers), loud only when the recording actually had a problem —
    // a hover-only tooltip would swallow a genuine finding.
    report_label_ = new QLabel(mode_bar_);
    report_label_->setObjectName(QStringLiteral("editReportLabel"));

    report_icon_ = new QLabel(mode_bar_);
    report_icon_->setObjectName(QStringLiteral("editReportIcon"));
    report_icon_->setFixedSize(20, 20);
    report_icon_->setAlignment(Qt::AlignCenter);

    mode_bar_layout->addWidget(back_btn_);
    mode_bar_layout->addWidget(title_label_);
    mode_bar_layout->addWidget(filename_label_, 1);
    mode_bar_layout->addStretch();
    mode_bar_layout->addWidget(report_label_);
    mode_bar_layout->addWidget(report_icon_);

    root_layout->addWidget(mode_bar_);

    // ---- Main Content Area (splitter-like HBox) ----
    auto* content_area = new QWidget(this);
    auto* content_layout = new QHBoxLayout(content_area);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    // ---- Left pane (player + timeline) ----
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

    // The surface fills the frame; the play/pause toggle floats centered over
    // it, and the meta label sits in the bottom-right corner. A single-cell
    // QGridLayout overlays all three (children added later paint on top), so the
    // play button lands in the middle of the video rectangle rather than pinned
    // to its top edge.
    auto* player_layout = new QGridLayout(player_frame_);
    player_layout->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
    // The single cell must absorb the frame's full height (the surface's size
    // hint alone would otherwise cap the row and push the overlay off-center).
    player_layout->setRowStretch(0, 1);
    player_layout->setColumnStretch(0, 1);

    player_surface_ = new ui::widgets::EditPlayerSurface(player_frame_);

    // 60px circular play/pause toggle. Drives the preview position clock and
    // the real decoder session (player_session_).
    play_pause_btn_ = new QPushButton(player_frame_);
    play_pause_btn_->setObjectName(QStringLiteral("editExportPlayPauseBtn"));
    play_pause_btn_->setFixedSize(60, 60);
    play_pause_btn_->setCursor(Qt::PointingHandCursor);
    play_pause_btn_->setIconSize(QSize(24, 24));
    play_pause_btn_->setToolTip(QStringLiteral("Play / pause preview"));

    player_meta_label_ = new QLabel(player_frame_);
    player_meta_label_->setObjectName(QStringLiteral("editExportPlayerMeta"));
    player_meta_label_->setAlignment(Qt::AlignRight | Qt::AlignBottom);

    player_layout->addWidget(player_surface_, 0, 0);
    player_layout->addWidget(play_pause_btn_, 0, 0, Qt::AlignCenter);
    player_layout->addWidget(player_meta_label_, 0, 0, Qt::AlignRight | Qt::AlignBottom);

    left_layout->addWidget(player_frame_);

    // Timeline: trim handles, markers, and the playhead live directly on the
    // strip — there is no button row or duration readout above it.
    timeline_ = new ui::widgets::EditTimeline(left_widget);
    timeline_->setObjectName(QStringLiteral("editTimeline"));
    left_layout->addWidget(timeline_);

    connect(timeline_, &ui::widgets::EditTimeline::trimHandleReleased, this, &EditExportPage::onTrimHandleReleased);
    connect(timeline_, &ui::widgets::EditTimeline::scrubStarted, this, &EditExportPage::onScrubStarted);
    connect(timeline_, &ui::widgets::EditTimeline::scrubMoved, this, &EditExportPage::onScrubMoved);
    connect(timeline_, &ui::widgets::EditTimeline::scrubFinished, this, &EditExportPage::onScrubFinished);

    left_layout->addStretch();

    left_scroll->setWidget(left_widget);

    // ---- Right pane: Details card (right-aligned mono values) ----
    auto* rail_column = new QWidget(content_area);
    rail_column->setFixedWidth(280);
    auto* rail_column_layout = new QVBoxLayout(rail_column);
    rail_column_layout->setContentsMargins(M::kSpaceSm, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    rail_column_layout->setSpacing(0);

    detail_rail_ = new ui::widgets::EditDetailsRail(rail_column);

    rail_column_layout->addWidget(detail_rail_);
    rail_column_layout->addStretch();

    content_layout->addWidget(left_scroll, 1);
    content_layout->addWidget(rail_column);

    root_layout->addWidget(content_area, 1);

    // ---- Bottom action bar: the Export action sits bottom-right, in the same
    // position the Record page keeps its primary transport actions. ----
    action_bar_ = new QFrame(this);
    action_bar_->setObjectName(QStringLiteral("editExportActionBar"));
    action_bar_->setFixedHeight(64);

    auto* action_layout = new QHBoxLayout(action_bar_);
    action_layout->setContentsMargins(M::kSpaceMd, 0, M::kSpaceMd, 0);
    action_layout->setSpacing(M::kSpaceSm);
    action_layout->addStretch();

    // The single action of the surface: everything else is direct manipulation
    // on the view itself.
    primary_action_btn_ = new QPushButton(QStringLiteral("Export…"), action_bar_);
    primary_action_btn_->setObjectName(QStringLiteral("editExportPrimaryBtn"));
    primary_action_btn_->setProperty("role", "primary");
    primary_action_btn_->setMinimumWidth(150);

    action_layout->addWidget(primary_action_btn_);

    root_layout->addWidget(action_bar_);

    // ---- Export card (over the view; presentation only) ----
    export_card_ = new ui::dialogs::ExportOverlay(this);

    // Wire signals
    connect(back_btn_, &QPushButton::clicked, this, &EditExportPage::onBackClicked);
    connect(play_pause_btn_, &QPushButton::clicked, this, [this]() { setPreviewPlaying(!preview_playing_); });
    connect(primary_action_btn_, &QPushButton::clicked, this, [this]() { export_card_->openCard(); });

    // The card decides what the user asked for; the page owns what actually
    // happens (confirmation, thread, sidecar, atomic rename).
    connect(export_card_, &ui::dialogs::ExportOverlay::exportRequested, this, &EditExportPage::onExportClicked);
    connect(export_card_, &ui::dialogs::ExportOverlay::cancelRequested, this, &EditExportPage::onCancelExportClicked);
    connect(export_card_, &ui::dialogs::ExportOverlay::retryRequested, this, &EditExportPage::onRetryExportClicked);
    connect(export_card_, &ui::dialogs::ExportOverlay::openFolderRequested, this, &EditExportPage::onOpenFolderClicked);
    connect(export_card_, &ui::dialogs::ExportOverlay::revealFileRequested, this, &EditExportPage::onRevealFileClicked);
    connect(export_card_, &ui::dialogs::ExportOverlay::closeRequested, this, [this]() { export_card_->closeCard(); });

    // Applies the theme-derived inline styling now, and re-applies it on every
    // theme switch so nothing keeps the old palette's colours or icon tints.
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

    // ---- Details card (right rail) / export card ----
    detail_rail_->applyThemeStyles();
    export_card_->applyThemeStyles();

    // ---- Bottom action bar ----
    action_bar_->setStyleSheet(QStringLiteral("QFrame#editExportActionBar {"
                                              "background:%1;"
                                              "border-top: 1px solid %2;"
                                              "}")
                                   .arg(t.surf, t.line));

    // The report icon's glyph and tint are severity-dependent, so they are
    // re-derived from ActiveTheme() here rather than baked at construction.
    refreshReportIcon();
}

// Quiet by default, visible when it matters: Good/Unavailable get a muted info
// glyph whose tooltip carries the three numbers; a Warning or Critical pipeline
// also gets a coloured triangle and a word beside it, so a real finding is not
// hidden behind a hover.
void EditExportPage::refreshReportIcon() {
    if (!report_icon_ || !report_label_)
        return;
    const auto& t = ActiveTheme();

    QString glyph = QStringLiteral("info");
    QColor tint = themeColor(t.dim);
    QString label;
    switch (report_severity_) {
    case ReportSeverity::Neutral:
        break;
    case ReportSeverity::Warning:
        glyph = QStringLiteral("alertTriangle");
        tint = themeColor(t.caution);
        label = QStringLiteral("Warning");
        break;
    case ReportSeverity::Critical:
        glyph = QStringLiteral("alertTriangle");
        tint = themeColor(t.error);
        label = QStringLiteral("Critical");
        break;
    }

    report_icon_->setPixmap(renderEditIcon(glyph, 16, tint));
    report_label_->setText(label);
    report_label_->setVisible(!label.isEmpty());
    report_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:11px; }").arg(tint.name(QColor::HexRgb)));

    const QString tooltip =
        QStringLiteral("%1\n%2\n%3").arg(report_drops_text_, report_drift_text_, report_health_text_);
    report_icon_->setToolTip(tooltip);
    report_label_->setToolTip(tooltip);
}

void EditExportPage::setEditContext(const EditContext& ctx) {
    ctx_ = ctx;
    setRecordingInfo(ctx_.output_path, ctx_.duration, ctx_.size, ctx_.resolution, ctx_.fps, ctx_.video_codec,
                     ctx_.audio_codec, ctx_.container);

    // --- Post-flight report (header icon + tooltip) ---
    const auto& snap = ctx_.completed_snapshot;
    const bool has_snap = snap.valid || snap.session_generation > 0;

    {
        // REAL drops only (encoder backpressure plus frame-processing failures) --
        // not deliberate CFR pacing/coalescing, which is intentional frame
        // selection (e.g. downsampling a 144 Hz source to a 60 fps CFR target),
        // not a drop. frames_dropped_problem() is the shared definition.
        const uint64_t total_dropped = snap.capture.frames_dropped_problem();
        const uint64_t total_frames = snap.capture.frames_emitted + total_dropped;
        if (has_snap && total_frames > 0) {
            const double pct = 100.0 * static_cast<double>(total_dropped) / static_cast<double>(total_frames);
            report_drops_text_ = QStringLiteral("Frame drops: %1%").arg(pct, 0, 'f', 1);
        } else {
            report_drops_text_ = QStringLiteral("Frame drops: \xe2\x80\x94");
        }
    }

    if (ctx_.av_drift_available) {
        report_drift_text_ =
            QStringLiteral("Peak A/V drift: \xc2\xb1%1\xc2\xa0ms").arg(ctx_.peak_av_drift_ms, 0, 'f', 0);
    } else {
        // No drift measurement is "no data" (same as MainWindow's own drift
        // readout), not a distinct state — show the unified empty-value glyph.
        report_drift_text_ = QStringLiteral("Peak A/V drift: \xe2\x80\x94");
    }

    report_severity_ = ReportSeverity::Neutral;
    if (has_snap) {
        const char* health_str = "Unknown";
        switch (snap.health) {
        case recorder_core::PipelineHealth::Good:
            health_str = "Good";
            break;
        case recorder_core::PipelineHealth::Warning:
            health_str = "Warning";
            report_severity_ = ReportSeverity::Warning;
            break;
        case recorder_core::PipelineHealth::Critical:
            health_str = "Critical";
            report_severity_ = ReportSeverity::Critical;
            break;
        case recorder_core::PipelineHealth::Unavailable:
            health_str = "Unavailable";
            break;
        default:
            break;
        }
        report_health_text_ = QStringLiteral("Pipeline health: %1").arg(QLatin1String(health_str));
    } else {
        report_health_text_ = QStringLiteral("Pipeline health: \xe2\x80\x94");
    }
    refreshReportIcon();

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
            player_session_->SetOnFrameReady([this](const recorder_core::DecodedVideoFrame& frame) {
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
    // The new clip's own frame rate drives the presentation cadence.
    refreshPreviewTickInterval();

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

    // Update detail rail. The rail renders an empty fact as the unified em dash,
    // so unset values need no special-casing here.
    if (detail_rail_) {
        ui::widgets::EditDetailsRail::Facts facts;
        facts.duration = duration_;
        facts.size = size_;
        facts.resolution = resolution_;
        facts.fps = fps_;
        facts.video_codec = video_codec_;
        facts.audio_codec = audio_codec_;
        facts.container = container_;
        detail_rail_->setFacts(facts);
    }

    // Update player meta
    if (player_meta_label_)
        player_meta_label_->setText(QStringLiteral("%1  %2  %3").arg(resolution_, fps_, container_));
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
        // Re-evaluated on every start: the window may have moved to a screen
        // with a different refresh rate since the clip was opened.
        refreshPreviewTickInterval();
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
    // No copy: the QImage keeps its own reference to the decoded buffer and
    // releases it via the cleanup hook. Copying instead would memcpy the whole
    // frame (~15 MB at 1440p) on the UI thread for every displayed frame.
    auto* keep_alive = new std::shared_ptr<const uint8_t[]>(frame.bgra);
    return QImage(
        frame.bgra.get(), static_cast<int>(frame.width), static_cast<int>(frame.height),
        static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32,
        [](void* owner) { delete static_cast<std::shared_ptr<const uint8_t[]>*>(owner); }, keep_alive);
}

void EditExportPage::refreshPreviewTickInterval() {
    if (!preview_timer_)
        return;
    const double clip_fps = player_session_ ? player_session_->VideoFrameRate() : 0.0;
    // The screen the window actually sits on, not the primary one -- dragging
    // ExoSnap from a 60 Hz to a 144 Hz panel should change the cadence.
    double screen_hz = 0.0;
    if (const QScreen* screen = window() ? window()->screen() : nullptr)
        screen_hz = screen->refreshRate();
    preview_timer_->setInterval(PreviewTickMsFor(clip_fps, screen_hz));
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

// True when closing would throw away work the user did on this surface. Markers
// count: the surface is where they become part of an exported clip, and a
// dismiss drops the retimed sidecar that export would have written.
bool EditExportPage::hasUnsavedEdits() const {
    const bool trimmed = trim_start_us_ != recorder_core::TrimRange::kNoTimestamp ||
                         trim_end_us_ != recorder_core::TrimRange::kNoTimestamp;
    return trimmed || !markers_.empty();
}

// Same shape and tone as confirmOverwrite(): the non-destructive choice is the
// default button, so a stray Enter never discards the edit.
bool EditExportPage::confirmDiscardEdits() {
    if (!hasUnsavedEdits())
        return true;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Discard edits"));
    box.setText(QStringLiteral("Discard edits?"));
    box.setInformativeText(QStringLiteral("The trim points and markers on this clip are not exported yet."));
    box.setIcon(QMessageBox::Warning);

    auto* keep_btn = box.addButton(QStringLiteral("Keep editing"), QMessageBox::RejectRole);
    auto* discard_btn = box.addButton(QStringLiteral("Discard"), QMessageBox::AcceptRole);
    box.setDefaultButton(keep_btn);
    box.exec();
    return box.clickedButton() == static_cast<QAbstractButton*>(discard_btn);
}

bool EditExportPage::eventFilter(QObject* obj, QEvent* event) {
    if (obj == player_frame_ && event->type() == QEvent::Resize) {
        // Defer to the next event-loop tick: setFixedHeight() from inside the
        // frame's own Resize delivery triggers a nested resize whose layout
        // pass is then overwritten when the original (stale-size) QResizeEvent
        // reaches the frame's QGridLayout — leaving the overlaid play button
        // positioned against the old height (off-center).
        QMetaObject::invokeMethod(this, &EditExportPage::updatePlayerHeight, Qt::QueuedConnection);
    }
    return QWidget::eventFilter(obj, event);
}

void EditExportPage::updatePlayerHeight() {
    // Aim for 16:9 relative to the player's current width, but cap the height so
    // the trim timeline below it stays reachable without scrolling — a real video
    // view letterboxes inside the frame anyway.
    if (!player_frame_)
        return;
    const int w = player_frame_->width();
    int target = qRound(w * 9.0 / 16.0);
    int reserved = 52 /* mode bar */ + 64 /* action bar */ + 2 * M::kSpaceMd;
    if (timeline_ && !timeline_->isHidden())
        reserved += timeline_->height() + M::kSpaceMd;
    const int max_h = std::max(180, height() - reserved);
    target = std::min(target, max_h);
    if (target > 0 && player_frame_->height() != target)
        player_frame_->setFixedHeight(target);
}

void EditExportPage::hideEvent(QHideEvent* event) {
    // Overlay dismissed / page hidden: the preview clock must not keep running.
    setPreviewPlaying(false);
    // A card left standing would come back on top of the next clip. closeCard()
    // is a no-op while an export runs, which is exactly right — that session is
    // still live behind the dismissed overlay.
    if (export_card_)
        export_card_->closeCard();
    // ...and neither must the decoder session's worker threads (or its WASAPI
    // renderer). setEditContext() opens a fresh session the next time the
    // overlay is shown for a clip.
    if (player_session_)
        player_session_->Close();
    QWidget::hideEvent(event);
}

// ---- Slots ----

void EditExportPage::onBackClicked() {
    if (!confirmDiscardEdits())
        return;
    emit backRequested();
}

void EditExportPage::onExportClicked() {
    if (!confirmOverwrite())
        return;
    runExport();
}

// "Overwrite original" replaces the recording this overlay was opened for, and
// the export finishes with an atomic replace -- once it succeeds no copy of the
// original is left anywhere on disk. So the question has to come before the
// export starts, not as a report afterwards.
//
// Deliberately not gated on std::filesystem::exists(): in overwrite mode the
// target IS that recording, so it is there by construction, and a probe would
// only add a branch that never runs.
bool EditExportPage::confirmOverwrite() {
    const bool overwrite = export_card_ && export_card_->saveModeKey() == QStringLiteral("overwrite");
    if (!overwrite)
        return true;

    const QString name = QFileInfo(ctx_.output_path).fileName();
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Overwrite original recording"));
    box.setText(name.isEmpty()
                    ? QStringLiteral("The original recording will be replaced by the exported result.")
                    : QStringLiteral("\xe2\x80\x9c%1\xe2\x80\x9d will be replaced by the exported result.").arg(name));
    box.setInformativeText(QStringLiteral("The original cannot be recovered afterwards."));
    box.setIcon(QMessageBox::Warning);

    auto* keep_btn = box.addButton(QStringLiteral("Keep original"), QMessageBox::RejectRole);
    auto* overwrite_btn = box.addButton(QStringLiteral("Overwrite"), QMessageBox::AcceptRole);
    // The destructive choice is never what a stray Enter key lands on.
    box.setDefaultButton(keep_btn);
    box.exec();
    return box.clickedButton() == static_cast<QAbstractButton*>(overwrite_btn);
}

void EditExportPage::onCancelExportClicked() {
    export_cancel_.store(true);
    // The background thread will detect the cancel and stop; the card snaps back
    // to its options immediately so the surface is usable again at once.
    export_running_ = false;
    if (export_card_)
        export_card_->openCard();
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
    export_running_ = true;
    if (export_card_)
        export_card_->showRunning();

    const QString container_key = export_card_ ? export_card_->containerKey() : QStringLiteral("mkv");
    const bool overwrite = export_card_ && export_card_->saveModeKey() == QStringLiteral("overwrite");
    const bool to_mp4 = (container_key == QStringLiteral("mp4"));

    if (ctx_.mkv_master_path.isEmpty()) {
        last_export_error_ = QStringLiteral("No edit master available for export.");
        export_running_ = false;
        if (export_card_)
            export_card_->showFailed(last_export_error_);
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
                    if (export_card_)
                        export_card_->setProgress(static_cast<int>(fraction * 100.0f));
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
                export_running_ = false;
                if (ok) {
                    // The card shows the real output path, not a placeholder.
                    if (export_card_)
                        export_card_->showDone(QString::fromStdWString(output_path.wstring()));
                    emit exportCompleted(QString::fromStdWString(output_path.wstring()));
                } else {
                    // ...and the real remuxer error, not a hardcoded one.
                    last_export_error_ = QString::fromStdString(err_msg);
                    if (export_card_)
                        export_card_->showFailed(last_export_error_);
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace exosnap
