// QuickControlPillWindow.cpp — QUICK-PILL-R1
//
// Interactive, capture-excluded quick-control pill overlay.
// Architecture: capture-excluded (WDA_EXCLUDEFROMCAPTURE) + interactive
// (NOT click-through — this is its own window class, distinct from class-1).
// See ADR 0016 and the QuickPill component in mappe-wave03.jsx.
//
// Spec (Mappe §0.3 / QuickPill):
//   bg: rgba(12,12,14,0.8)  border: rgba(255,255,255,0.16)  radius: 16  padding: 8
//   Grip: 28px, rgba(255,255,255,0.5)
//   Buttons: 44×44px, radius 12, bg rgba(255,255,255,0.06), border rgba(255,255,255,0.1)
//   Stop button (rec-styled): bg rgba(224,120,108,0.18), border rgba(224,120,108,0.5), coral glyph #E0786C
//   Glyph size: 18px nominal
//
// MARKER button: deferred to 0.11.0 (markers wave) — NOT rendered here.

#include "QuickControlPillWindow.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QShowEvent>

#if defined(Q_OS_WIN)
#include <windows.h>
#if !defined(WDA_EXCLUDEFROMCAPTURE)
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace exosnap::ui::overlay {

namespace {

// ── Design-system tokens (Mappe QuickPill) ───────────────────────────────────
constexpr QColor kPillBg{12, 12, 14, 204};       // rgba(12,12,14,0.8)
constexpr QColor kPillBorder{255, 255, 255, 41}; // rgba(255,255,255,0.16)
constexpr QColor kPillRadius = {};               // placeholder; radius is an int below
constexpr int kRadius = 16;
constexpr int kPadding = 8;

// Button: normal
constexpr QColor kBtnBg{255, 255, 255, 15};     // rgba(255,255,255,0.06)
constexpr QColor kBtnBorder{255, 255, 255, 26}; // rgba(255,255,255,0.1)
constexpr QColor kBtnGlyph{255, 255, 255, 230}; // rgba(255,255,255,0.9)

// Button: stop / rec-styled (coral)
constexpr QColor kStopBtnBg{224, 120, 108, 46};        // rgba(224,120,108,0.18)
constexpr QColor kStopBtnBorder{224, 120, 108, 128};   // rgba(224,120,108,0.5)
constexpr QColor kStopBtnGlyph{0xE0, 0x78, 0x6C, 255}; // #E0786C

// Grip
constexpr QColor kGripColor{255, 255, 255, 128}; // rgba(255,255,255,0.5)

// Geometry
constexpr int kGripW = 28;
constexpr int kBtnSize = 44;
constexpr int kBtnRadius = 12;
constexpr int kBtnGap = 8; // gap between buttons
constexpr int kGlyphSz = 18;

// Positioning: bottom-center of primary screen
constexpr int kMarginBottom = 32;

// Icon IDs (internal enum, drawn as QPainter vector graphics)
enum class IconId { Pause, Resume, Stop, Camera, GripLines };

// ── Vector glyph drawing ─────────────────────────────────────────────────────

static void drawIcon(QPainter& p, IconId id, QRectF area, QColor color) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, 1.6f, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    const float cx = static_cast<float>(area.center().x());
    const float cy = static_cast<float>(area.center().y());
    const float s = static_cast<float>(kGlyphSz) * 0.5f;

    switch (id) {
    case IconId::Pause: {
        // Two vertical bars (pause symbol)
        const float bw = s * 0.30f;
        const float bh = s * 0.80f;
        const float gap = s * 0.22f;
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(cx - gap * 0.5f - bw, cy - bh, bw, bh * 2.0f), 1.5f, 1.5f);
        p.drawRoundedRect(QRectF(cx + gap * 0.5f, cy - bh, bw, bh * 2.0f), 1.5f, 1.5f);
        break;
    }
    case IconId::Resume: {
        // Right-pointing triangle (play symbol)
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF tri;
        tri << QPointF(cx - s * 0.3f, cy - s * 0.6f) << QPointF(cx + s * 0.6f, cy)
            << QPointF(cx - s * 0.3f, cy + s * 0.6f);
        p.drawPolygon(tri);
        break;
    }
    case IconId::Stop: {
        // Rounded square (stop symbol) — filled
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const float sqw = s * 0.85f;
        p.drawRoundedRect(QRectF(cx - sqw * 0.5f, cy - sqw * 0.5f, sqw, sqw), 3.0f, 3.0f);
        break;
    }
    case IconId::Camera: {
        // Camera body + lens
        const float bw = s * 1.3f;
        const float bh = s * 1.0f;
        const float br = 3.0f;
        const QRectF body(cx - bw * 0.5f, cy - bh * 0.5f + s * 0.1f, bw, bh);
        p.drawRoundedRect(body, br, br);

        // Lens circle
        p.drawEllipse(QPointF(cx, cy + s * 0.1f), s * 0.35f, s * 0.35f);

        // Viewfinder bump on top
        const float vfw = s * 0.5f;
        const float vfh = s * 0.25f;
        p.drawRoundedRect(QRectF(cx - vfw * 0.5f, cy - bh * 0.5f - vfh * 0.5f + s * 0.1f, vfw, vfh), 2.0f, 2.0f);
        break;
    }
    case IconId::GripLines: {
        // Three short horizontal dots / lines arranged vertically (grip symbol)
        p.setPen(QPen(color, 1.8f, Qt::SolidLine, Qt::RoundCap));
        const float lw = s * 0.5f;
        const float spacing = s * 0.45f;
        for (int i = -1; i <= 1; ++i) {
            p.drawLine(QPointF(cx - lw, cy + i * spacing), QPointF(cx + lw, cy + i * spacing));
        }
        break;
    }
    }
    p.restore();
}

} // namespace

// ---------------------------------------------------------------------------

QuickControlPillWindow::QuickControlPillWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint) {
    // KEY DISTINCTION: no Qt::WindowTransparentForInput — this window is interactive.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);
    // Accept mouse events for drag and button clicks.
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);

    // Default to hidden; updateVisibility() drives show/hide.
    hide();
}

void QuickControlPillWindow::updateState(bool recording_active, bool paused) {
    recording_active_ = recording_active;
    paused_ = paused;
    updateVisibility();
    if (isVisible())
        update();
}

void QuickControlPillWindow::setShowQuickControls(bool show) {
    show_quick_controls_ = show;
    updateVisibility();
}

bool QuickControlPillWindow::isExcluded() const noexcept {
    return excluded_;
}

void QuickControlPillWindow::setExpanded(bool expanded) {
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    resize(sizeHint());
    updatePosition();
    update();
}

bool QuickControlPillWindow::isExpanded() const noexcept {
    return expanded_;
}

QSize QuickControlPillWindow::sizeHint() const {
    // Collapsed: grip only
    // Expanded: grip + 3 buttons + gaps between them
    const int w_collapsed = kPadding + kGripW + kPadding;
    const int h = kPadding + kBtnSize + kPadding;

    if (!expanded_)
        return QSize(w_collapsed, h);

    // 3 buttons: Pause/Resume, Stop, CaptureFrame
    const int n_buttons = 3;
    const int w_expanded = w_collapsed + kBtnGap + n_buttons * kBtnSize + (n_buttons - 1) * kBtnGap;
    return QSize(w_expanded, h);
}

QSize QuickControlPillWindow::minimumSizeHint() const {
    return sizeHint();
}

// ── Geometry helpers ─────────────────────────────────────────────────────────

QRect QuickControlPillWindow::gripRect() const {
    const int h = height();
    return QRect(kPadding, 0, kGripW, h);
}

QRect QuickControlPillWindow::pauseResumeButtonRect() const {
    if (!expanded_)
        return QRect();
    const int x = kPadding + kGripW + kBtnGap;
    const int y = kPadding;
    return QRect(x, y, kBtnSize, kBtnSize);
}

QRect QuickControlPillWindow::stopButtonRect() const {
    if (!expanded_)
        return QRect();
    const int x = kPadding + kGripW + kBtnGap + kBtnSize + kBtnGap;
    const int y = kPadding;
    return QRect(x, y, kBtnSize, kBtnSize);
}

QRect QuickControlPillWindow::captureFrameButtonRect() const {
    if (!expanded_)
        return QRect();
    const int x = kPadding + kGripW + kBtnGap + 2 * (kBtnSize + kBtnGap);
    const int y = kPadding;
    return QRect(x, y, kBtnSize, kBtnSize);
}

// ── Paint ────────────────────────────────────────────────────────────────────

void QuickControlPillWindow::paintEvent(QPaintEvent* /*event*/) {
    const QRect rect = this->rect();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // ── Pill background ───────────────────────────────────────────────────────
    QPainterPath pill;
    pill.addRoundedRect(rect, kRadius, kRadius);
    p.fillPath(pill, kPillBg);
    p.setPen(QPen(kPillBorder, 1.0));
    p.drawPath(pill);

    // ── Grip handle ───────────────────────────────────────────────────────────
    const QRect gr = gripRect();
    // Cursor: Qt doesn't support per-region cursor changes with pure QPainter;
    // we set grab cursor on mouse-over in mousePressEvent instead.
    drawIcon(p, IconId::GripLines, QRectF(gr), kGripColor);

    if (!expanded_)
        return;

    // ── Action buttons ────────────────────────────────────────────────────────

    // Pause / Resume
    {
        const QRect r = pauseResumeButtonRect();
        const bool rec_style = false;
        p.setPen(Qt::NoPen);
        p.setBrush(rec_style ? kStopBtnBg : kBtnBg);
        p.drawRoundedRect(r, kBtnRadius, kBtnRadius);
        p.setPen(QPen(rec_style ? kStopBtnBorder : kBtnBorder, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, kBtnRadius, kBtnRadius);

        const IconId icon = paused_ ? IconId::Resume : IconId::Pause;
        drawIcon(p, icon, QRectF(r), kBtnGlyph);
    }

    // Stop (rec-styled)
    {
        const QRect r = stopButtonRect();
        p.setPen(Qt::NoPen);
        p.setBrush(kStopBtnBg);
        p.drawRoundedRect(r, kBtnRadius, kBtnRadius);
        p.setPen(QPen(kStopBtnBorder, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, kBtnRadius, kBtnRadius);
        drawIcon(p, IconId::Stop, QRectF(r), kStopBtnGlyph);
    }

    // Capture frame
    {
        const QRect r = captureFrameButtonRect();
        p.setPen(Qt::NoPen);
        p.setBrush(kBtnBg);
        p.drawRoundedRect(r, kBtnRadius, kBtnRadius);
        p.setPen(QPen(kBtnBorder, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, kBtnRadius, kBtnRadius);
        drawIcon(p, IconId::Camera, QRectF(r), kBtnGlyph);
    }

    // MARKER button: deferred to 0.11.0 (markers wave) — NOT rendered.
}

// ── Mouse events ─────────────────────────────────────────────────────────────

void QuickControlPillWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint local = event->pos();
    const QRect gr = gripRect();

    if (gr.contains(local)) {
        // Grip area: begin drag tracking.
        dragging_ = true;
        drag_start_local_ = local;
        drag_start_global_ = event->globalPosition().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // Button hit tests
    if (expanded_) {
        if (pauseResumeButtonRect().contains(local)) {
            emit pauseResumeRequested();
            event->accept();
            return;
        }
        if (stopButtonRect().contains(local)) {
            emit stopRequested();
            event->accept();
            return;
        }
        if (captureFrameButtonRect().contains(local)) {
            emit captureFrameRequested();
            event->accept();
            return;
        }
    }

    QWidget::mousePressEvent(event);
}

void QuickControlPillWindow::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        const QPoint delta = event->globalPosition().toPoint() - drag_start_global_;
        move(pos() + delta);
        drag_start_global_ = event->globalPosition().toPoint();
        event->accept();
        return;
    }

    // Update cursor based on hover region
    const QPoint local = event->pos();
    if (gripRect().contains(local)) {
        setCursor(Qt::OpenHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }

    QWidget::mouseMoveEvent(event);
}

void QuickControlPillWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        setCursor(Qt::ArrowCursor);

        // If not moved significantly, treat as grip click → toggle expand
        const QPoint delta = event->globalPosition().toPoint() - drag_start_global_;
        if (delta.manhattanLength() <= 4 && gripRect().contains(event->pos())) {
            setExpanded(!expanded_);
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void QuickControlPillWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            hide();
        }
    }
}

// ── Capture exclusion ────────────────────────────────────────────────────────

void QuickControlPillWindow::applyExclusion() {
    exclusion_attempted_ = true;
    excluded_ = false;

#if defined(Q_OS_WIN)
    // winId() forces HWND creation.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    // WDA_EXCLUDEFROMCAPTURE (0x11) requires Windows 10 2004+.
    // NOTE: Unlike RecordingOverlayWindow we do NOT set WS_EX_TRANSPARENT.
    // This window accepts mouse input — it is interactive.
    const BOOL ok = ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    excluded_ = (ok != FALSE);
#else
    excluded_ = false;
#endif
}

// ── Visibility gating ────────────────────────────────────────────────────────

void QuickControlPillWindow::updateVisibility() {
    const bool should_show = show_quick_controls_ && recording_active_;

    if (should_show) {
        if (!exclusion_attempted_) {
            // Force HWND creation so we can apply exclusion before show.
            winId();
            applyExclusion();
        }
        if (!excluded_) {
            // Exclusion failed — must not show; could contaminate recording.
            hide();
            return;
        }
        resize(sizeHint());
        if (!isVisible()) {
            updatePosition();
            show();
            raise();
        } else {
            update();
        }
    } else {
        hide();
    }
}

// ── Positioning ──────────────────────────────────────────────────────────────

void QuickControlPillWindow::updatePosition() {
    const QSize hint = sizeHint();
    resize(hint);

    // Default: bottom-center of the primary screen.
    const QScreen* primary = QGuiApplication::primaryScreen();
    QRect screen_rect;
    if (primary)
        screen_rect = primary->geometry();

    if (screen_rect.isNull() || screen_rect.isEmpty()) {
        move(0, 0);
        return;
    }

    const int x = screen_rect.left() + (screen_rect.width() - hint.width()) / 2;
    const int y = screen_rect.bottom() - hint.height() - kMarginBottom;
    move(x, y);
}

} // namespace exosnap::ui::overlay
