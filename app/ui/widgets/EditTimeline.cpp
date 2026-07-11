#include "EditTimeline.h"

#include "../../models/EditTimelineModel.h"
#include "../theme/ExoSnapTheme.h"

#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

namespace {

// Vertical layout: a quiet zone above the track for the drag time label and
// the playhead knob, the 52px track itself, and a mono time row underneath.
constexpr int kLabelZoneH = 22;
constexpr int kTrackH = 52;
constexpr int kTimeRowGap = 6;
constexpr int kTimeRowH = 14;
constexpr int kSideInset = 5; // keeps the centred handles/knob unclipped at 0%/100%

constexpr int kTrackRadius = 10;
constexpr int kHandleW = 8;
constexpr int kHandleWActive = 10; // "leicht gescaled" while dragging
constexpr int kHandleOverhang = 2; // handles extend past the track edges
constexpr int kHandleHitSlop = 4;
constexpr int kPlayheadKnobD = 10;
constexpr int kWaveformBars = 72;

QFont monoFont(int pixel_size) {
    QFont font(QStringLiteral("IBM Plex Mono"));
    font.setPixelSize(pixel_size);
    return font;
}

} // namespace

EditTimeline::EditTimeline(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kLabelZoneH + kTrackH + kTimeRowGap + kTimeRowH);
    setMouseTracking(true); // hover cursor over handles/playhead
}

void EditTimeline::setDurationMs(qint64 duration_ms) {
    duration_ms_ = std::max<qint64>(duration_ms, 0);
    trim_start_ms_ = 0;
    trim_end_ms_ = duration_ms_;
    position_ms_ = 0;
    update();
}

void EditTimeline::setMarkers(std::vector<RecordingMarker> markers) {
    markers_ = std::move(markers);
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
    return {kSideInset, kLabelZoneH, width() - 2 * kSideInset, kTrackH};
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
    // Anywhere else on (or above) the track scrubs the playhead.
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
    const QColor ink = ParseThemeColor(theme.ink);

    const QRect track = trackRect();
    QPainterPath track_path;
    track_path.addRoundedRect(QRectF(track).adjusted(0.5, 0.5, -0.5, -0.5), kTrackRadius, kTrackRadius);

    // Track base + border.
    p.setPen(QPen(line2, 1.0));
    p.setBrush(surf2);
    p.drawPath(track_path);

    // Everything inside the track clips to its rounded shape.
    p.save();
    p.setClipPath(track_path);

    // Pseudo-waveform bars (deterministic — mirrors the design suite's
    // sin-derived heights so fixtures render identically everywhere).
    {
        p.setPen(Qt::NoPen);
        QColor bar = accent;
        bar.setAlphaF(0.20f);
        p.setBrush(bar);
        const double inner_left = track.left() + 4.0;
        const double inner_width = track.width() - 8.0;
        const double slot = inner_width / kWaveformBars;
        const double bar_w = std::max(slot - 2.0, 1.0);
        for (int i = 0; i < kWaveformBars; ++i) {
            const double h = 12.0 + (std::sin(i * 1.7) + 1.0) * 14.0;
            const double x = inner_left + i * slot;
            const double y = track.center().y() - h / 2.0;
            p.drawRoundedRect(QRectF(x, y, bar_w, h), 1.0, 1.0);
        }
    }

    const bool interactive = duration_ms_ > 0;

    // Trimmed-away ranges are dimmed.
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

    // Markers: thin caution-coloured verticals across the full track height —
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
