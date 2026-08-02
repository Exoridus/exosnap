#include "EditTimeline.h"

#include "../../models/EditTimelineModel.h"
#include "../theme/ExoSnapTheme.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

namespace {

// Vertical layout: a quiet zone above the stack for the drag time label and the
// playhead knob, the row stack itself, and a mono time row underneath.
constexpr int kLabelZoneH = 22;
constexpr int kVideoRowH = 40;
constexpr int kAudioRowH = 20;
// Floor for a shared audio row. Below this the label no longer fits, and a row
// too small to name is worse than one row fewer.
constexpr int kAudioRowMinH = 12;
constexpr int kRowGap = 2;
// The audio rows share this much height between them, so a recording with many
// tracks cannot squeeze the player out at the 700 px minimum window height.
// Sized for three nominal rows plus their gaps — the most the shipped audio
// model produces (APP, SYS, MIC unmerged).
constexpr int kAudioStackBudgetH = 3 * kAudioRowH + 2 * kRowGap;
constexpr int kTimeRowGap = 6;
constexpr int kTimeRowH = 14;
constexpr int kSideInset = 5; // keeps the centred handles/knob unclipped at 0%/100%

constexpr int kTrackRadius = 10;
constexpr int kHandleW = 8;
constexpr int kHandleWActive = 10; // "leicht gescaled" while dragging
constexpr int kHandleOverhang = 2; // handles extend past the track edges
constexpr int kHandleHitSlop = 4;
constexpr int kPlayheadKnobD = 10;
constexpr int kAudioLabelInset = 8;

// A resize changes the tile count, so every intermediate width of a drag would
// otherwise start (and cancel) its own decode run.
constexpr int kResizeDebounceMs = 150;

QFont monoFont(int pixel_size) {
    QFont font(QStringLiteral("IBM Plex Mono"));
    font.setPixelSize(pixel_size);
    return font;
}

// Placeholder tile for the visual harness and the widget tests, which have no
// clip to decode. Deterministic per position so a fixture renders identically
// on every machine.
QImage fixtureTile(int index, int width, int height) {
    QImage tile(std::max(width, 1), std::max(height, 1), QImage::Format_ARGB32);
    const int shade = 40 + (index * 17) % 90;
    tile.fill(QColor(shade, shade + 6, shade + 10));
    return tile;
}

} // namespace

EditTimeline::EditTimeline(QWidget* parent) : QWidget(parent) {
    resize_debounce_ = new QTimer(this);
    resize_debounce_->setSingleShot(true);
    resize_debounce_->setInterval(kResizeDebounceMs);
    connect(resize_debounce_, &QTimer::timeout, this, &EditTimeline::requestThumbnails);

    updateHeight();
    setMouseTracking(true); // hover cursor over handles/playhead
}

EditTimeline::~EditTimeline() = default;

int EditTimeline::videoRowHeight() const noexcept {
    return kVideoRowH;
}

int EditTimeline::audioRowHeight() const noexcept {
    const int rows = static_cast<int>(audio_track_labels_.size());
    if (rows <= 0)
        return 0;
    const int budget = kAudioStackBudgetH - (rows - 1) * kRowGap;
    return std::clamp(budget / rows, kAudioRowMinH, kAudioRowH);
}

int EditTimeline::preferredHeight() const noexcept {
    const int rows = static_cast<int>(audio_track_labels_.size());
    const int audio_stack = rows > 0 ? rows * (kRowGap + audioRowHeight()) : 0;
    return kLabelZoneH + kVideoRowH + audio_stack + kTimeRowGap + kTimeRowH;
}

void EditTimeline::updateHeight() {
    const int target = preferredHeight();
    if (height() == target && minimumHeight() == target)
        return;
    setFixedHeight(target);
}

void EditTimeline::setDurationMs(qint64 duration_ms) {
    duration_ms_ = std::max<qint64>(duration_ms, 0);
    trim_start_ms_ = 0;
    trim_end_ms_ = duration_ms_;
    position_ms_ = 0;
    // The tile positions are spread over the clip's own length, so a new
    // duration invalidates the strip that was decoded for the old one.
    thumbnails_.clear();
    requestThumbnails();
    update();
}

void EditTimeline::setMarkers(std::vector<RecordingMarker> markers) {
    markers_ = std::move(markers);
    update();
}

void EditTimeline::setAudioTrackLabels(const QStringList& labels) {
    if (audio_track_labels_ == labels)
        return;
    audio_track_labels_ = labels;
    updateHeight();
    update();
}

void EditTimeline::setClip(const QString& path, std::vector<int64_t> keyframes_us) {
    clip_path_ = path;
    keyframes_us_ = std::move(keyframes_us);
    thumbnails_.clear();
    thumbnail_fixture_.reset();
    clip_video_width_ = 0;
    clip_video_height_ = 0;
    if (clip_path_.isEmpty() && !thumbnails_source_) {
        // Never opened a clip and is not being given one: nothing to close, and
        // no reason to start a worker thread.
        update();
        return;
    }
    if (!thumbnails_source_) {
        // Created on first use: a page that never opens a clip (and every test
        // that only exercises the trim maths) never starts a worker thread.
        thumbnails_source_ = new TimelineThumbnailSource(this);
        connect(thumbnails_source_, &TimelineThumbnailSource::clipOpened, this,
                [this](int video_width, int video_height, const QStringList& audio_track_names) {
                    clip_video_width_ = video_width;
                    clip_video_height_ = video_height;
                    // One row per track the clip actually carries. Names come
                    // from the container; empty ones are labelled positionally
                    // when the row is painted.
                    setAudioTrackLabels(audio_track_names);
                    // The tile count follows the clip's aspect ratio, so the
                    // first real run can only start once the file is open.
                    requestThumbnails();
                });
        connect(thumbnails_source_, &TimelineThumbnailSource::tileReady, this,
                [this](qint64 time_ms, const QImage& image, quint64 run_id) {
                    if (run_id != thumbnail_run_)
                        return; // a run this widget's geometry has moved past
                    thumbnails_.push_back(TimelineThumbnail{time_ms, image});
                    update();
                });
    }
    thumbnails_source_->openClip(clip_path_, keyframes_us_);
    update();
}

void EditTimeline::setThumbnailFixture(int count) {
    thumbnail_fixture_ = count;
    if (thumbnails_source_)
        thumbnails_source_->cancel();
    requestThumbnails();
}

std::vector<qint64> EditTimeline::thumbnailTimesMs() const {
    const int tile_w = TimelineTileWidth(videoRowHeight(), clip_video_width_, clip_video_height_);
    const int tiles = TimelineTileCount(trackRect().width(), tile_w);
    return TimelineTileTimesMs(tiles, duration_ms_, keyframes_us_);
}

void EditTimeline::requestThumbnails() {
    const std::vector<qint64> times = thumbnailTimesMs();

    if (thumbnail_fixture_) {
        const int requested = *thumbnail_fixture_;
        const int tiles =
            requested < 0 ? static_cast<int>(times.size()) : std::min<int>(requested, static_cast<int>(times.size()));
        const int tile_w = TimelineTileWidth(videoRowHeight(), clip_video_width_, clip_video_height_);
        thumbnails_.clear();
        thumbnails_.reserve(static_cast<size_t>(std::max(tiles, 0)));
        for (int i = 0; i < tiles; ++i)
            thumbnails_.push_back(
                TimelineThumbnail{times[static_cast<size_t>(i)], fixtureTile(i, tile_w, videoRowHeight())});
        update();
        return;
    }

    if (!thumbnails_source_ || times.empty())
        return;
    thumbnails_.clear();
    thumbnail_run_ = thumbnails_source_->requestTiles(times, videoRowHeight());
    update();
}

void EditTimeline::setTrimRangeMs(qint64 start_ms, qint64 end_ms) {
    if (duration_ms_ <= 0)
        return;
    trim_end_ms_ = ClampTrimEndMs(end_ms, 0, duration_ms_);
    trim_start_ms_ = ClampTrimStartMs(start_ms, trim_end_ms_);
    update();
}

void EditTimeline::setPositionMs(qint64 position_ms) {
    const qint64 clamped = ClampPlayheadMs(position_ms, duration_ms_);
    if (clamped == position_ms_)
        return;
    position_ms_ = clamped;
    update();
}

QRect EditTimeline::trackRect() const {
    const int rows = static_cast<int>(audio_track_labels_.size());
    const int audio_stack = rows > 0 ? rows * (kRowGap + audioRowHeight()) : 0;
    return {kSideInset, kLabelZoneH, width() - 2 * kSideInset, kVideoRowH + audio_stack};
}

QRect EditTimeline::videoRowRect() const {
    const QRect track = trackRect();
    return {track.left(), track.top(), track.width(), kVideoRowH};
}

QRect EditTimeline::audioRowRect(int index) const {
    const QRect track = trackRect();
    const int row_h = audioRowHeight();
    const int top = track.top() + kVideoRowH + index * (kRowGap + row_h) + kRowGap;
    return {track.left(), top, track.width(), row_h};
}

int EditTimeline::xForMs(qint64 ms) const {
    const QRect track = trackRect();
    if (duration_ms_ <= 0 || track.width() <= 0)
        return track.left();
    const double fraction = std::clamp(static_cast<double>(ms) / static_cast<double>(duration_ms_), 0.0, 1.0);
    return track.left() + static_cast<int>(std::lround(fraction * track.width()));
}

qint64 EditTimeline::msForX(int x) const {
    const QRect track = trackRect();
    if (duration_ms_ <= 0 || track.width() <= 0)
        return 0;
    const double fraction =
        std::clamp(static_cast<double>(x - track.left()) / static_cast<double>(track.width()), 0.0, 1.0);
    return static_cast<qint64>(std::llround(fraction * static_cast<double>(duration_ms_)));
}

QRect EditTimeline::handleRect(qint64 ms, bool active) const {
    const QRect track = trackRect();
    const int w = active ? kHandleWActive : kHandleW;
    const int overhang = active ? kHandleOverhang + 1 : kHandleOverhang;
    return {xForMs(ms) - w / 2, track.top() - overhang, w, track.height() + 2 * overhang};
}

EditTimeline::DragTarget EditTimeline::hitTest(const QPoint& pos) const {
    if (duration_ms_ <= 0)
        return DragTarget::None;
    const int slop = kHandleHitSlop;
    // Handles win over the playhead: they are the rarer, more deliberate grab.
    if (handleRect(trim_start_ms_, false).adjusted(-slop, -slop, slop, slop).contains(pos))
        return DragTarget::TrimStart;
    if (handleRect(trim_end_ms_, false).adjusted(-slop, -slop, slop, slop).contains(pos))
        return DragTarget::TrimEnd;
    // Anywhere else on (or above) the stack scrubs the playhead.
    const QRect track = trackRect();
    if (pos.y() >= track.top() - kPlayheadKnobD && pos.y() <= track.bottom() + kHandleOverhang)
        return DragTarget::Playhead;
    return DragTarget::None;
}

void EditTimeline::updateHoverCursor(const QPoint& pos) {
    switch (hitTest(pos)) {
    case DragTarget::TrimStart:
    case DragTarget::TrimEnd:
        setCursor(Qt::SizeHorCursor);
        break;
    case DragTarget::Playhead:
        setCursor(Qt::PointingHandCursor);
        break;
    case DragTarget::None:
        unsetCursor();
        break;
    }
}

void EditTimeline::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->size().width() == event->oldSize().width())
        return;
    if (thumbnail_fixture_) {
        // Placeholder tiles cost nothing to build — there is no decode to
        // debounce, and deferring would leave a harness capture blank.
        requestThumbnails();
        return;
    }
    resize_debounce_->start();
}

void EditTimeline::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || duration_ms_ <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    drag_ = hitTest(event->pos());
    switch (drag_) {
    case DragTarget::TrimStart:
    case DragTarget::TrimEnd:
        break;
    case DragTarget::Playhead: {
        // A press on the track jumps the playhead there and begins a scrub.
        emit scrubStarted();
        position_ms_ = ClampPlayheadMs(msForX(event->pos().x()), duration_ms_);
        emit scrubMoved(position_ms_);
        break;
    }
    case DragTarget::None:
        QWidget::mousePressEvent(event);
        return;
    }
    update();
    event->accept();
}

void EditTimeline::mouseMoveEvent(QMouseEvent* event) {
    if (drag_ == DragTarget::None) {
        updateHoverCursor(event->pos());
        QWidget::mouseMoveEvent(event);
        return;
    }
    const qint64 at = msForX(event->pos().x());
    switch (drag_) {
    case DragTarget::TrimStart:
        trim_start_ms_ = ClampTrimStartMs(at, trim_end_ms_);
        emit trimRangeEdited(trim_start_ms_, trim_end_ms_);
        break;
    case DragTarget::TrimEnd:
        trim_end_ms_ = ClampTrimEndMs(at, trim_start_ms_, duration_ms_);
        emit trimRangeEdited(trim_start_ms_, trim_end_ms_);
        break;
    case DragTarget::Playhead:
        position_ms_ = ClampPlayheadMs(at, duration_ms_);
        emit scrubMoved(position_ms_);
        break;
    case DragTarget::None:
        break;
    }
    update();
    event->accept();
}

void EditTimeline::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || drag_ == DragTarget::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const DragTarget finished = drag_;
    drag_ = DragTarget::None;
    switch (finished) {
    case DragTarget::TrimStart:
    case DragTarget::TrimEnd:
        emit trimHandleReleased(trim_start_ms_, trim_end_ms_);
        break;
    case DragTarget::Playhead:
        emit scrubFinished();
        break;
    case DragTarget::None:
        break;
    }
    updateHoverCursor(event->pos());
    update();
    event->accept();
}

void EditTimeline::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const auto& theme = ActiveTheme();
    const QColor surf2 = ParseThemeColor(theme.surf2);
    const QColor line2 = ParseThemeColor(theme.line2);
    const QColor bg = ParseThemeColor(theme.bg);
    const QColor accent = ParseThemeColor(theme.ac);
    const QColor caution = ParseThemeColor(theme.caution);
    const QColor dim = ParseThemeColor(theme.dim);
    const QColor mut = ParseThemeColor(theme.mut);
    const QColor ink = ParseThemeColor(theme.ink);

    const QRect track = trackRect();
    QPainterPath track_path;
    track_path.addRoundedRect(QRectF(track).adjusted(0.5, 0.5, -0.5, -0.5), kTrackRadius, kTrackRadius);

    // Track base + border.
    p.setPen(QPen(line2, 1.0));
    p.setBrush(surf2);
    p.drawPath(track_path);

    // Everything inside the stack clips to its rounded shape.
    p.save();
    p.setClipPath(track_path);

    // Video row: decoded thumbnails, each drawn left-aligned at its own
    // timestamp. Positions without a tile stay empty — a placeholder pattern
    // would be information the clip never gave.
    {
        const QRect row = videoRowRect();
        p.save();
        p.setClipRect(row, Qt::IntersectClip);
        for (const auto& tile : thumbnails_) {
            if (tile.image.isNull())
                continue;
            p.drawImage(QPoint(xForMs(tile.time_ms), row.top()), tile.image);
        }
        p.restore();
    }

    // Audio rows: a label over a fill, and deliberately nothing else. Drawing a
    // waveform here would need the whole soundtrack decoded, and approximating
    // one is the invented shape this timeline stopped drawing.
    {
        const int rows = static_cast<int>(audio_track_labels_.size());
        const QFont label_font = monoFont(10);
        QColor fill = accent;
        fill.setAlphaF(0.14f);
        for (int i = 0; i < rows; ++i) {
            const QRect row = audioRowRect(i);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRect(row);
            // A hairline against the row above, so the lanes read as separate
            // tracks rather than as one tinted block.
            p.setBrush(line2);
            p.drawRect(QRect(row.left(), row.top() - kRowGap, row.width(), 1));

            QString label = audio_track_labels_.at(i);
            if (label.isEmpty())
                label = QStringLiteral("Audio %1").arg(i + 1);
            p.setPen(mut);
            p.setFont(label_font);
            p.drawText(row.adjusted(kAudioLabelInset, 0, -kAudioLabelInset, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       label);
        }
    }

    const bool interactive = duration_ms_ > 0;

    // Trimmed-away ranges are dimmed across every row: a trim applies to the
    // whole clip, not to one of its tracks.
    if (interactive && isTrimmed()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 8, 10, 168));
        const int in_x = xForMs(trim_start_ms_);
        const int out_x = xForMs(trim_end_ms_);
        if (in_x > track.left())
            p.drawRect(QRect(track.left(), track.top(), in_x - track.left(), track.height()));
        if (out_x < track.right())
            p.drawRect(QRect(out_x, track.top(), track.right() - out_x + 1, track.height()));
    }

    // Markers: thin caution-coloured verticals across the full stack height —
    // the same reading as the record timeline in the design suite.
    if (interactive) {
        p.setPen(Qt::NoPen);
        p.setBrush(caution);
        for (const auto& marker : markers_) {
            const int x = xForMs(static_cast<qint64>(marker.time_ms));
            p.drawRect(QRect(x - 1, track.top(), 2, track.height()));
        }
    }

    p.restore(); // end track clip

    // Trim handles (accent pill with a background ring so they read over any
    // track content).
    if (interactive) {
        const bool start_active = (drag_ == DragTarget::TrimStart);
        const bool end_active = (drag_ == DragTarget::TrimEnd);
        const auto drawHandle = [&](qint64 ms, bool active) {
            const QRect r = handleRect(ms, active);
            p.setPen(QPen(bg, 2.0));
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(r), 4.0, 4.0);
        };
        drawHandle(trim_start_ms_, start_active);
        drawHandle(trim_end_ms_, end_active);

        // Playhead: white line with a knob at its top.
        const bool playhead_active = (drag_ == DragTarget::Playhead);
        const int px = xForMs(position_ms_);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));
        p.drawRect(QRect(px - 1, track.top() - 3, 2, track.height() + 6));
        const int knob = playhead_active ? kPlayheadKnobD + 2 : kPlayheadKnobD;
        p.drawEllipse(QPointF(px, track.top() - 3), knob / 2.0, knob / 2.0);

        // Drag feedback: a centred mono time label above the active element.
        if (drag_ != DragTarget::None) {
            qint64 label_ms = position_ms_;
            int label_x = px;
            if (drag_ == DragTarget::TrimStart) {
                label_ms = trim_start_ms_;
                label_x = xForMs(trim_start_ms_);
            } else if (drag_ == DragTarget::TrimEnd) {
                label_ms = trim_end_ms_;
                label_x = xForMs(trim_end_ms_);
            }
            const QString text = FormatTimelineTimestamp(label_ms, duration_ms_);
            const QFont font = monoFont(10);
            const QFontMetrics fm(font);
            const int text_w = fm.horizontalAdvance(text);
            const int pill_w = text_w + 12;
            const int pill_h = 16;
            int pill_x = label_x - pill_w / 2;
            pill_x = std::clamp(pill_x, 0, std::max(width() - pill_w, 0));
            const QRectF pill(pill_x, 2, pill_w, pill_h);
            p.setPen(QPen(line2, 1.0));
            p.setBrush(bg);
            p.drawRoundedRect(pill, 6.0, 6.0);
            p.setPen(ink);
            p.setFont(font);
            p.drawText(pill, Qt::AlignCenter, text);
        }
    }

    // Static time row: clip start · in/out readout while trimmed · duration.
    {
        const QFont font = monoFont(10);
        p.setFont(font);
        p.setPen(dim);
        const QRect row(track.left(), track.bottom() + kTimeRowGap, track.width(), kTimeRowH);
        p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, FormatTimelineClock(0, duration_ms_));
        p.drawText(row, Qt::AlignRight | Qt::AlignVCenter, FormatTimelineClock(duration_ms_, duration_ms_));
        if (isTrimmed()) {
            const QString mid = QStringLiteral("in %1 \xc2\xb7 out %2")
                                    .arg(FormatTimelineClock(trim_start_ms_, duration_ms_),
                                         FormatTimelineClock(trim_end_ms_, duration_ms_));
            p.drawText(row, Qt::AlignHCenter | Qt::AlignVCenter, mid);
        }
    }
}

} // namespace exosnap::ui::widgets
