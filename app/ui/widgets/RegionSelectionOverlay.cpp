// RegionSelectionOverlay.cpp — REGION-SELECTION-SKIN-R1
//
// Visual spec from Mappe wave03 RegionMock:
//   • Selection rect: 1.5 px border, accent #9BD9D2, corner radius 4.
//   • Scrim: rgba(8,8,10,0.5) painted over the whole overlay; selection rect
//     is punched through so the content shows.  The effective scrim outside the
//     selection therefore matches rgba(8,8,10,0.45–0.55).
//   • Corner handles: 13×13 px squares, radius 4, accent fill, 2 px bg-color
//     border drawn at the 4 corners.
//   • Live readout: "W × H · ratio" in IBM Plex Mono 11 pt above the top-left
//     of the rect. While dragging the rect shows dimensions; no-selection shows
//     "Drag to select".
//   • Confirm pill: accent fill, dark ink text, radius 999, padding 4×11 px.
//   • Esc pill: white text, rgba(12,12,14,0.8) bg, 1 px white/16 border.
//   • Aspect-ratio snapping: when the ratio is within 5 % of a preset (16:9,
//     4:3, 1:1, 21:9), a subtle hint label appears near the readout.
//
// The accept/cancel signal flow (regionSelected / regionCancelled) is preserved
// exactly.  Only the visual rendering and button labels change.

#include "RegionSelectionOverlay.h"

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>

#include "RegionGeometry.h"

#include <algorithm>
#include <cmath>

namespace exosnap::ui::widgets {

// ── Design tokens ────────────────────────────────────────────────────────────

static constexpr QRgb kAccent = RegionSelectionOverlay::kAccentRgb;     // #9BD9D2
static constexpr QRgb kAccentI = RegionSelectionOverlay::kAccentInkRgb; // #08130F
static constexpr QRgb kBg = RegionSelectionOverlay::kBgRgb;             // #0E0E10

static inline QColor accentColor() {
    return QColor(kAccent);
}
static inline QColor bgColor() {
    return QColor(kBg);
}
static inline QColor accentInkColor() {
    return QColor(kAccentI);
}

// Scrim applied over the whole overlay (outside selection punched through).
static inline QColor scrimColor() {
    return QColor(8, 8, 10, 140);
} // ~0.55 alpha

// Readout / Esc pill background.
static inline QColor pillBgColor() {
    return QColor(12, 12, 14, 204);
} // rgba(12,12,14,0.8)

// Esc pill border.
static inline QColor escBorderColor() {
    return QColor(255, 255, 255, 41);
} // rgba(255,255,255,0.16)

// Handle size and radius (spec: 13 px, radius 4).
static constexpr int kHandleSize = 13;
static constexpr int kHandleRadius = 4;
// Handle border width (spec: 2 px in bg color).
static constexpr int kHandleBorder = 2;

// Readout font size in points.
static constexpr int kReadoutFontPt = 9; // ~11 px on a 96-dpi screen (Qt uses points)

// Aspect snap threshold percent.
static constexpr int kSnapThresholdPct = 5;

// ── QSS-styled buttons ───────────────────────────────────────────────────────
// Buttons are styled via QSS loaded by the global theme; we also apply a
// minimal inline style so the buttons look correct even without QSS.

static void styleConfirmButton(QPushButton* btn) {
    const QString accent = QString::fromLatin1("#9BD9D2");
    const QString ink = QString::fromLatin1("#08130F");
    btn->setStyleSheet(QString::fromLatin1("QPushButton {"
                                           "  background: %1;"
                                           "  color: %2;"
                                           "  border: none;"
                                           "  border-radius: 999px;"
                                           "  padding: 4px 11px;"
                                           "  font-size: 11px;"
                                           "  font-weight: 600;"
                                           "}"
                                           "QPushButton:hover { background: #b0e0da; }"
                                           "QPushButton:pressed { background: #88ccc5; }")
                           .arg(accent, ink));
}

static void styleEscButton(QPushButton* btn) {
    btn->setStyleSheet(QString::fromLatin1("QPushButton {"
                                           "  background: rgba(12,12,14,0.80);"
                                           "  color: #ffffff;"
                                           "  border: 1px solid rgba(255,255,255,0.16);"
                                           "  border-radius: 999px;"
                                           "  padding: 4px 11px;"
                                           "  font-size: 11px;"
                                           "  font-weight: 400;"
                                           "}"
                                           "QPushButton:hover { background: rgba(30,30,34,0.90); }"
                                           "QPushButton:pressed { background: rgba(8,8,10,0.95); }"));
}

// ── Constructor ──────────────────────────────────────────────────────────────

RegionSelectionOverlay::RegionSelectionOverlay(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);

    confirm_button_ = new QPushButton(QStringLiteral("Confirm"), this);
    confirm_button_->setObjectName(QStringLiteral("regionOverlayConfirmButton"));
    confirm_button_->setProperty("role", QStringLiteral("primary"));
    confirm_button_->setVisible(false);
    confirm_button_->setCursor(Qt::PointingHandCursor);
    styleConfirmButton(confirm_button_);
    connect(confirm_button_, &QPushButton::clicked, this, &RegionSelectionOverlay::confirmSelection);

    // Label changed from "Cancel" to "Esc" per Mappe spec.
    cancel_button_ = new QPushButton(QStringLiteral("Esc"), this);
    cancel_button_->setObjectName(QStringLiteral("regionOverlayCancelButton"));
    cancel_button_->setProperty("role", QStringLiteral("ghost"));
    cancel_button_->setVisible(false);
    cancel_button_->setCursor(Qt::PointingHandCursor);
    styleEscButton(cancel_button_);
    connect(cancel_button_, &QPushButton::clicked, this, &RegionSelectionOverlay::cancelSelection);
}

// ── Public API ───────────────────────────────────────────────────────────────

void RegionSelectionOverlay::activateForSelection(QRect monitor_virtual_screen, QRect initial_region_virtual) {
    dragging_ = false;
    drag_start_ = {};
    drag_current_ = {};
    drag_last_ = {};
    drag_origin_rect_ = {};
    active_handle_ = RegionResizeHandle::None;
    setOverlayInteraction(InteractionMode::None);

    // Compute the union of all screen geometries (virtual desktop).
    QRect virtualRect;
    const auto screens = QGuiApplication::screens();
    for (const auto* screen : screens) {
        virtualRect = virtualRect.united(screen->geometry());
    }
    if (virtualRect.isEmpty()) {
        if (QGuiApplication::primaryScreen()) {
            virtualRect = QGuiApplication::primaryScreen()->geometry();
        }
    }
    if (virtualRect.isEmpty()) {
        cancelSelection();
        return;
    }

    if (!monitor_virtual_screen.isValid() || monitor_virtual_screen.isEmpty()) {
        monitor_virtual_screen = virtualRect;
    }

    setGeometry(virtualRect);
    monitor_rect_ = toLocal(monitor_virtual_screen).intersected(rect());
    if (!monitor_rect_.isValid() || monitor_rect_.width() < kMinDimension || monitor_rect_.height() < kMinDimension) {
        monitor_rect_ = rect();
    }

    initial_region_ = toLocal(initial_region_virtual).intersected(monitor_rect_);
    editing_existing_ = initial_region_.isValid() && initial_region_.width() >= kMinDimension &&
                        initial_region_.height() >= kMinDimension;
    selection_rect_ = editing_existing_ ? ClampRegionToMonitor(initial_region_, monitor_rect_, kMinDimension) : QRect();

    if (confirm_button_)
        confirm_button_->setVisible(selection_rect_.isValid());
    if (cancel_button_)
        cancel_button_->setVisible(true);
    updateActionGeometry();

    show();
    raise();
    activateWindow();
    setFocus();
    grabKeyboard();
    updateCursorForPosition(mapFromGlobal(QCursor::pos()));
    update();
}

// ── Static helpers ───────────────────────────────────────────────────────────

// Common preset ratios: {numerator, denominator, label}.
struct PresetRatio {
    int num;
    int den;
    const char* label;
};

static constexpr PresetRatio kPresets[] = {
    {16, 9, "16:9"},
    {21, 9, "21:9"},
    {4, 3, "4:3"},
    {1, 1, "1:1"},
};

static double aspectOf(int w, int h) {
    return h > 0 ? static_cast<double>(w) / static_cast<double>(h) : 0.0;
}

// Returns the label of the nearest preset if the given ratio is within
// threshold_pct % of that preset, otherwise empty string.
QString RegionSelectionOverlay::nearestPresetLabel(int width, int height, int threshold_pct) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    const double ratio = aspectOf(width, height);
    for (const auto& p : kPresets) {
        const double target = aspectOf(p.num, p.den);
        const double diff = std::abs(ratio - target) / target * 100.0;
        if (diff <= static_cast<double>(threshold_pct)) {
            return QString::fromLatin1(p.label);
        }
    }
    return {};
}

// Format: "1280 × 720 · 16:9"  (middle dot U+00B7, multiplication sign × U+00D7)
QString RegionSelectionOverlay::formatReadout(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    // Use QChar escapes so the Unicode code points are embedded directly into the
    // QString without going through fromLatin1 (which misinterprets multi-byte
    // UTF-8 sequences) or relying on the source-file encoding.
    //   × = U+00D7 MULTIPLICATION SIGN
    //   · = U+00B7 MIDDLE DOT
    static const QChar kTimes(0x00D7);
    static const QChar kDot(0x00B7);
    const QString ratio_label = nearestPresetLabel(width, height);
    if (!ratio_label.isEmpty()) {
        return QStringLiteral("%1 %2 %3 %4 %5").arg(width).arg(kTimes).arg(height).arg(kDot).arg(ratio_label);
    }
    return QStringLiteral("%1 %2 %3").arg(width).arg(kTimes).arg(height);
}

// Snap: if the current ratio is within threshold_pct % of a preset, adjust the
// width so the rect matches the preset ratio exactly (keeping the left+top corner
// fixed, clamped to the monitor).
QRect RegionSelectionOverlay::snapToPresetAspect(const QRect& sel, const QRect& monitor, int snap_threshold_pct) {
    if (!sel.isValid() || sel.width() <= 0 || sel.height() <= 0) {
        return sel;
    }
    const double ratio = aspectOf(sel.width(), sel.height());
    for (const auto& p : kPresets) {
        const double target = aspectOf(p.num, p.den);
        const double diff = std::abs(ratio - target) / target * 100.0;
        if (diff <= static_cast<double>(snap_threshold_pct)) {
            // Snap width to match the preset ratio for the current height.
            const int snapped_w =
                static_cast<int>(std::round(sel.height() * static_cast<double>(p.num) / static_cast<double>(p.den)));
            if (snapped_w <= 0) {
                return sel;
            }
            // Clamp to monitor right edge.
            const int max_w = monitor.right() - sel.left() + 1;
            const int new_w = std::min(snapped_w, max_w);
            return QRect(sel.topLeft(), QSize(new_w, sel.height()));
        }
    }
    return sel;
}

// ── Geometry helpers ─────────────────────────────────────────────────────────

QRect RegionSelectionOverlay::selectionRectLocal() const {
    return dragging_ && interaction_mode_ == InteractionMode::Selecting ? QRect(drag_start_, drag_current_).normalized()
                                                                        : selection_rect_;
}

QRect RegionSelectionOverlay::monitorRectLocal() const {
    return monitor_rect_.isValid() ? monitor_rect_ : rect();
}

QRect RegionSelectionOverlay::toLocal(const QRect& virtual_screen_rect) const {
    return virtual_screen_rect.translated(-geometry().topLeft());
}

QRect RegionSelectionOverlay::toVirtual(const QRect& local_rect) const {
    return local_rect.translated(geometry().topLeft());
}

RegionResizeHandle RegionSelectionOverlay::hitTestHandle(const QPoint& pos) const {
    if (!selection_rect_.isValid()) {
        return RegionResizeHandle::None;
    }
    constexpr int kHandleHit = 18; // larger hit area than the 13 px visual handle
    auto nearPoint = [pos](const QPoint& p) {
        return QRect(p.x() - kHandleHit / 2, p.y() - kHandleHit / 2, kHandleHit, kHandleHit).contains(pos);
    };
    if (nearPoint(selection_rect_.topLeft()))
        return RegionResizeHandle::TopLeft;
    if (nearPoint(QPoint(selection_rect_.right(), selection_rect_.top())))
        return RegionResizeHandle::TopRight;
    if (nearPoint(QPoint(selection_rect_.left(), selection_rect_.bottom())))
        return RegionResizeHandle::BottomLeft;
    if (nearPoint(selection_rect_.bottomRight()))
        return RegionResizeHandle::BottomRight;
    return RegionResizeHandle::None;
}

bool RegionSelectionOverlay::hitTestMove(const QPoint& pos) const {
    return selection_rect_.isValid() && selection_rect_.contains(pos);
}

void RegionSelectionOverlay::setOverlayInteraction(InteractionMode mode) {
    if (interaction_mode_ == mode) {
        return;
    }
    interaction_mode_ = mode;
    emit interactionModeChanged(mode);
}

void RegionSelectionOverlay::updateCursorForPosition(const QPoint& pos) {
    if (dragging_) {
        return;
    }
    switch (hitTestHandle(pos)) {
    case RegionResizeHandle::TopLeft:
    case RegionResizeHandle::BottomRight:
        setCursor(Qt::SizeFDiagCursor);
        return;
    case RegionResizeHandle::TopRight:
    case RegionResizeHandle::BottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        return;
    case RegionResizeHandle::None:
        break;
    }
    setCursor(hitTestMove(pos) ? Qt::SizeAllCursor : Qt::CrossCursor);
}

void RegionSelectionOverlay::updateActionGeometry() {
    if (!confirm_button_ || !cancel_button_) {
        return;
    }

    constexpr int kGap = 7; // gap between Esc and Confirm pills (matches Mappe)
    confirm_button_->adjustSize();
    cancel_button_->adjustSize();
    const int total_width = confirm_button_->width() + cancel_button_->width() + kGap;

    QPoint pos;
    if (selection_rect_.isValid()) {
        // Below-right of the selection rect (Mappe: bottom-right of the rect).
        pos = QPoint(selection_rect_.right() - total_width, selection_rect_.bottom() + 10);
        // Push up if below the screen.
        if (pos.y() + confirm_button_->height() > rect().bottom()) {
            pos.setY(selection_rect_.top() - confirm_button_->height() - 10);
        }
    } else {
        pos = monitorRectLocal().topLeft() + QPoint(16, 16);
    }
    pos.setX(std::clamp(pos.x(), rect().left() + 8, rect().right() - total_width - 8));
    pos.setY(std::clamp(pos.y(), rect().top() + 8, rect().bottom() - confirm_button_->height() - 8));

    // Layout: [Esc] [Confirm] — Esc on left, Confirm on right, matching Mappe order.
    cancel_button_->move(pos);
    confirm_button_->move(pos.x() + cancel_button_->width() + kGap, pos.y());
}

// ── Paint helpers ─────────────────────────────────────────────────────────────

void RegionSelectionOverlay::paintScrimAndSelection(QPainter& p, const QRect& sel) const {
    // Step 1: Fill the entire overlay with the scrim color.
    p.fillRect(rect(), scrimColor());

    // Step 2: Punch the selection rect through to show content beneath.
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    QPainterPath punch;
    punch.addRoundedRect(sel, 4, 4);
    p.fillPath(punch, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Step 3: Draw the selection border (1.5 px accent, radius 4).
    QPen selPen(accentColor(), 1.5);
    selPen.setJoinStyle(Qt::RoundJoin);
    p.setPen(selPen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(sel, 4, 4);
}

void RegionSelectionOverlay::paintCornerHandles(QPainter& p, const QRect& sel) const {
    // Handles: 13×13 squares, radius 4, accent fill with 2 px bg-color border.
    // Positioned so their center aligns with the corner of the rect (offset by half).
    const int hs = kHandleSize;
    const int off = hs / 2; // 6

    const QPoint corners[4] = {
        sel.topLeft(),
        QPoint(sel.right(), sel.top()),
        QPoint(sel.left(), sel.bottom()),
        sel.bottomRight(),
    };

    for (const QPoint& c : corners) {
        const QRectF handle(c.x() - off, c.y() - off, hs, hs);

        // Bg-color border: draw a slightly larger rounded rect in bg color first.
        const qreal borderExt = kHandleBorder;
        const QRectF outer(handle.adjusted(-borderExt, -borderExt, borderExt, borderExt));
        p.setPen(Qt::NoPen);
        p.setBrush(bgColor());
        p.drawRoundedRect(outer, kHandleRadius + borderExt, kHandleRadius + borderExt);

        // Accent fill.
        p.setPen(Qt::NoPen);
        p.setBrush(accentColor());
        p.drawRoundedRect(handle, kHandleRadius, kHandleRadius);
    }
}

void RegionSelectionOverlay::paintReadout(QPainter& p, const QRect& sel) const {
    const QString text = formatReadout(sel.width(), sel.height());
    if (text.isEmpty()) {
        return;
    }

    QFont font(QStringLiteral("IBM Plex Mono"));
    font.setPointSize(kReadoutFontPt);
    p.setFont(font);

    const QFontMetrics fm(font);
    const int padH = 8;
    const int padV = 2;
    const int tw = fm.horizontalAdvance(text);
    const int th = fm.height();
    const int bw = tw + padH * 2;
    const int bh = th + padV * 2;

    // Position above the top-left of the selection rect.
    const int gap = 6;
    int bx = sel.left();
    int by = sel.top() - bh - gap;
    if (by < rect().top() + 4) {
        by = sel.bottom() + gap;
    }
    bx = std::clamp(bx, rect().left() + 4, rect().right() - bw - 4);

    const QRect bg(bx, by, bw, bh);

    p.setPen(Qt::NoPen);
    p.setBrush(pillBgColor());
    p.drawRoundedRect(bg, 6, 6);

    p.setPen(Qt::white);
    p.drawText(bg, Qt::AlignCenter, text);
}

void RegionSelectionOverlay::paintAspectHint(QPainter& p, const QRect& sel, const QString& hint_label) const {
    if (hint_label.isEmpty()) {
        return;
    }
    // Subtle label shown just to the right of the readout area, in dim accent color.
    QFont font(QStringLiteral("IBM Plex Mono"));
    font.setPointSize(kReadoutFontPt - 1);
    p.setFont(font);

    const int padH = 6;
    const int padV = 2;
    const QFontMetrics fm(font);
    const int tw = fm.horizontalAdvance(hint_label);
    const int th = fm.height();
    const int bw = tw + padH * 2;
    const int bh = th + padV * 2;

    // Position above the rect, to the right of the readout.
    const int gap = 6;
    const int by = std::max(rect().top() + 4, sel.top() - bh - gap);
    const int bx = std::clamp(sel.right() - bw, rect().left() + 4, rect().right() - bw - 4);

    const QRect bg(bx, by, bw, bh);

    // Subtle: semi-transparent accent background.
    QColor hintBg(kAccent);
    hintBg.setAlpha(30);
    p.setPen(Qt::NoPen);
    p.setBrush(hintBg);
    p.drawRoundedRect(bg, 4, 4);

    QColor hintText(kAccent);
    hintText.setAlpha(200);
    p.setPen(hintText);
    p.drawText(bg, Qt::AlignCenter, hint_label);
}

void RegionSelectionOverlay::paintInstruction(QPainter& p) const {
    const QRect monitor = monitorRectLocal();
    p.setPen(QColor(255, 255, 255, 180));
    QFont f(QStringLiteral("IBM Plex Mono"));
    f.setPointSize(kReadoutFontPt + 1);
    p.setFont(f);
    p.drawText(monitor, Qt::AlignCenter, QStringLiteral("Drag to select a capture region\nPress Esc to cancel"));
}

// ── paintEvent ───────────────────────────────────────────────────────────────

void RegionSelectionOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRect sel = selectionRectLocal();

    if (sel.isValid() && sel.width() >= 1 && sel.height() >= 1) {
        // Scrim + punch-through + selection border.
        paintScrimAndSelection(p, sel);

        // Corner handles.
        paintCornerHandles(p, sel);

        // Live readout.
        paintReadout(p, sel);

        // Aspect hint (separate label when snapping is near but not yet exact).
        const QString hint = nearestPresetLabel(sel.width(), sel.height(), kSnapThresholdPct);
        // Only show the hint when the readout doesn't already contain the ratio.
        // (If formatReadout included it, the hint would be redundant.)
        // Since formatReadout already includes it when near, paint the hint only
        // when the selection is very close (within 1 %) but the readout isn't a
        // perfect match — skip here to avoid double-display; the readout is enough.
        // We still call paintAspectHint as a hook for a potential "snap!" indicator.
        // For now emit no separate hint (it's baked into the readout).
        Q_UNUSED(hint);

        // "Too small" rejection hint.
        if (sel.width() < kMinDimension || sel.height() < kMinDimension) {
            p.setPen(QColor(255, 100, 100));
            QFont hf(QStringLiteral("IBM Plex Mono"));
            hf.setPointSize(kReadoutFontPt);
            p.setFont(hf);
            p.drawText(sel, Qt::AlignCenter, QStringLiteral("Too small"));
        }
    } else {
        // No selection yet: full scrim + instruction.
        p.fillRect(rect(), scrimColor());
        paintInstruction(p);
    }
}

// ── Mouse events ─────────────────────────────────────────────────────────────

void RegionSelectionOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (confirm_button_ && confirm_button_->geometry().contains(event->pos()))
        return;
    if (cancel_button_ && cancel_button_->geometry().contains(event->pos()))
        return;

    const QRect monitor = monitorRectLocal();
    const QPoint pos(std::clamp(event->pos().x(), monitor.left(), monitor.right()),
                     std::clamp(event->pos().y(), monitor.top(), monitor.bottom()));
    active_handle_ = hitTestHandle(pos);
    drag_last_ = pos;
    drag_origin_rect_ = selection_rect_;
    dragging_ = true;

    if (active_handle_ != RegionResizeHandle::None) {
        switch (active_handle_) {
        case RegionResizeHandle::TopLeft:
            setOverlayInteraction(InteractionMode::ResizingTopLeft);
            break;
        case RegionResizeHandle::TopRight:
            setOverlayInteraction(InteractionMode::ResizingTopRight);
            break;
        case RegionResizeHandle::BottomLeft:
            setOverlayInteraction(InteractionMode::ResizingBottomLeft);
            break;
        case RegionResizeHandle::BottomRight:
            setOverlayInteraction(InteractionMode::ResizingBottomRight);
            break;
        case RegionResizeHandle::None:
            break;
        }
    } else if (hitTestMove(pos)) {
        setOverlayInteraction(InteractionMode::Moving);
    } else {
        drag_start_ = pos;
        drag_current_ = pos;
        selection_rect_ = {};
        setOverlayInteraction(InteractionMode::Selecting);
    }
    update();
}

void RegionSelectionOverlay::mouseMoveEvent(QMouseEvent* event) {
    const QRect monitor = monitorRectLocal();
    const QPoint pos(std::clamp(event->pos().x(), monitor.left(), monitor.right()),
                     std::clamp(event->pos().y(), monitor.top(), monitor.bottom()));

    if (!dragging_) {
        updateCursorForPosition(pos);
        return;
    }

    if (interaction_mode_ == InteractionMode::Moving) {
        selection_rect_ = MoveRegionWithinMonitor(drag_origin_rect_, pos - drag_last_, monitor);
    } else if (active_handle_ != RegionResizeHandle::None) {
        selection_rect_ = ResizeRegionFromCorner(drag_origin_rect_, active_handle_, pos, monitor, kMinDimension);
    } else {
        drag_current_ = pos;
        selection_rect_ = QRect(drag_start_, drag_current_).normalized().intersected(monitor);
    }
    if (confirm_button_) {
        confirm_button_->setVisible(selection_rect_.isValid() && selection_rect_.width() >= kMinDimension &&
                                    selection_rect_.height() >= kMinDimension);
    }
    updateActionGeometry();
    update();
}

void RegionSelectionOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_)
        return;

    dragging_ = false;
    const QRect sel = selectionRectLocal();

    if (sel.width() < kMinDimension || sel.height() < kMinDimension) {
        if (editing_existing_ && initial_region_.isValid()) {
            selection_rect_ = initial_region_;
            setOverlayInteraction(InteractionMode::None);
            updateActionGeometry();
            update();
            return;
        }
        cancelSelection();
        return;
    }

    selection_rect_ = ClampRegionToMonitor(sel, monitorRectLocal(), kMinDimension);
    setOverlayInteraction(InteractionMode::None);
    if (confirm_button_)
        confirm_button_->setVisible(true);
    if (cancel_button_)
        cancel_button_->setVisible(true);
    updateActionGeometry();
    updateCursorForPosition(event->pos());
    update();
}

// ── Confirm / cancel ─────────────────────────────────────────────────────────

void RegionSelectionOverlay::confirmSelection() {
    if (!selection_rect_.isValid() || selection_rect_.width() < kMinDimension ||
        selection_rect_.height() < kMinDimension) {
        return;
    }
    const QRect virtualRect = toVirtual(selection_rect_);
    releaseMouse();
    releaseKeyboard();
    hide();
    setOverlayInteraction(InteractionMode::None);
    emit regionSelected(virtualRect);
}

void RegionSelectionOverlay::cancelSelection() {
    dragging_ = false;
    releaseMouse();
    releaseKeyboard();
    hide();
    setOverlayInteraction(InteractionMode::None);
    emit regionCancelled();
}

// ── Key events ───────────────────────────────────────────────────────────────

void RegionSelectionOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancelSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        confirmSelection();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RegionSelectionOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateActionGeometry();
}

} // namespace exosnap::ui::widgets
