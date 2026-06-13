// CountdownOverlayWindow.cpp — COUNTDOWN-OVERLAY-R1
//
// Class-1 (capture-excluded, click-through) countdown overlay.
//
// Mappe wave03 CountdownOverlay spec:
//   124px circle, bg rgba(12,12,14,0.74), border rgba(255,255,255,0.16)
//   outer ring + drop shadow (approximated with the opaque dark bg)
//   Depleting ring: r=57, stroke 3px
//     track  rgba(255,255,255,0.12)
//     progress #E6C57C (amber), round linecap, starts at 12 o'clock
//   Digit: IBM Plex Mono, 52px, weight 500, #E6C57C, tabular-nums, centered
//   Centered on the recorded monitor (inset:0)

#include "CountdownOverlayWindow.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QShowEvent>
#include <cmath>

#if defined(Q_OS_WIN)
#include <windows.h>
// WDA_EXCLUDEFROMCAPTURE was introduced in Windows 10 2004 (build 19041).
// The constant may not be defined in older SDKs.
#if !defined(WDA_EXCLUDEFROMCAPTURE)
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace exosnap::ui::overlay {

namespace {

// ── Mappe tokens ──────────────────────────────────────────────────────────────
// circle bg: rgba(12,12,14,0.74) → 0.74 × 255 ≈ 189
constexpr QColor kCircleBg{12, 12, 14, 189};
// border: rgba(255,255,255,0.16) → 0.16 × 255 ≈ 41
constexpr QColor kBorderColor{255, 255, 255, 41};
// outer ring: rgba(0,0,0,0.5) → shadow
constexpr QColor kOuterRing{0, 0, 0, 128};
// drop shadow: rgba(0,0,0,0.5)
constexpr QColor kShadow{0, 0, 0, 128};
// ring track: rgba(255,255,255,0.12) → 0.12 × 255 ≈ 31
constexpr QColor kRingTrack{255, 255, 255, 31};
// ring progress + digit: amber #E6C57C
constexpr QColor kAmber{0xE6, 0xC5, 0x7C, 255};

// Circle geometry (Mappe: 124px diameter, ring radius 57, stroke 3)
constexpr int kCircleSize = 124;
constexpr qreal kRingRadius = 57.0;
constexpr qreal kRingStroke = 3.0;

// Canvas size: add shadow margin so the shadow isn't clipped
constexpr int kShadowMargin = 18;
constexpr int kCanvasSize = kCircleSize + kShadowMargin * 2;

// Digit font
constexpr qreal kFontPx = 52.0;

// Pi
static constexpr qreal kPi = 3.14159265358979323846;

} // namespace

CountdownOverlayWindow::CountdownOverlayWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                          Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);
    resize(sizeHint());
}

void CountdownOverlayWindow::showCountdown(int remaining_seconds, int duration_seconds) {
    remaining_seconds_ = (remaining_seconds > 0) ? remaining_seconds : 1;
    duration_seconds_ = (duration_seconds > 0) ? duration_seconds : remaining_seconds_;

    if (!exclusion_attempted_) {
        applyExclusion();
    }
    if (!excluded_) {
        // Exclusion failed — refuse to show; overlay must not contaminate recording.
        return;
    }

    updatePosition();
    update();
    show();
    raise();
}

void CountdownOverlayWindow::updateCountdown(int remaining_seconds, int duration_seconds) {
    remaining_seconds_ = (remaining_seconds > 0) ? remaining_seconds : 1;
    duration_seconds_ = (duration_seconds > 0) ? duration_seconds : remaining_seconds_;

    if (isVisible())
        update();
}

void CountdownOverlayWindow::hideOverlay() {
    hide();
}

void CountdownOverlayWindow::setMonitorGeometry(const QRect& monitor_rect) {
    monitor_rect_ = monitor_rect;
    if (isVisible())
        updatePosition();
}

bool CountdownOverlayWindow::isExcluded() const noexcept {
    return excluded_;
}

int CountdownOverlayWindow::remainingSeconds() const noexcept {
    return remaining_seconds_;
}

int CountdownOverlayWindow::durationSeconds() const noexcept {
    return duration_seconds_;
}

void CountdownOverlayWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            hide();
        }
    }
}

void CountdownOverlayWindow::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const qreal cx = kCanvasSize / 2.0;
    const qreal cy = kCanvasSize / 2.0;
    const qreal r = kCircleSize / 2.0;

    // ── Drop shadow (approximating box-shadow: 0 12px 40px rgba(0,0,0,0.5)) ───
    {
        const qreal spread = 10.0;
        const QRectF sh_rect(cx - r - spread, cy - r - spread + 6, (r + spread) * 2, (r + spread) * 2);
        QPainterPath sh_path;
        sh_path.addEllipse(sh_rect);
        p.fillPath(sh_path, kShadow);
    }

    // ── Outer ring (box-shadow: 0 0 0 1px rgba(0,0,0,0.5)) ─────────────────
    {
        QPainterPath ring_path;
        ring_path.addEllipse(QRectF(cx - r - 1, cy - r - 1, (r + 1) * 2, (r + 1) * 2));
        p.fillPath(ring_path, kOuterRing);
    }

    // ── Circle background ─────────────────────────────────────────────────────
    {
        QPainterPath circ;
        circ.addEllipse(QRectF(cx - r, cy - r, r * 2, r * 2));
        p.fillPath(circ, kCircleBg);
    }

    // ── Border 1px rgba(255,255,255,0.16) ────────────────────────────────────
    {
        QPen border_pen(kBorderColor);
        border_pen.setWidthF(1.0);
        p.setPen(border_pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(cx - r + 0.5, cy - r + 0.5, r * 2 - 1, r * 2 - 1));
    }

    // ── Ring track (full circle) rgba(255,255,255,0.12) ──────────────────────
    {
        QPen track_pen(kRingTrack);
        track_pen.setWidthF(kRingStroke);
        track_pen.setCapStyle(Qt::RoundCap);
        p.setPen(track_pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(cx - kRingRadius, cy - kRingRadius, kRingRadius * 2, kRingRadius * 2));
    }

    // ── Ring progress (depleting, amber, starts at 12 o'clock) ───────────────
    // Progress = remaining / duration (1.0 = full, 0.0 = empty).
    {
        const qreal progress = (duration_seconds_ > 0)
                                   ? static_cast<qreal>(remaining_seconds_) / static_cast<qreal>(duration_seconds_)
                                   : 1.0;
        // Clamp to [0, 1]
        const qreal clamped = (progress < 0.0) ? 0.0 : (progress > 1.0) ? 1.0 : progress;

        if (clamped > 0.0) {
            // Qt drawArc: start angle in 1/16th degrees, -90° = 12 o'clock,
            // span in 1/16th degrees, positive = counter-clockwise.
            // We want clockwise depletion: span = -360 * progress (negative = CW in Qt).
            const qreal sweep_deg = -360.0 * clamped;

            QPen prog_pen(kAmber);
            prog_pen.setWidthF(kRingStroke);
            prog_pen.setCapStyle(Qt::RoundCap);
            p.setPen(prog_pen);
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(cx - kRingRadius, cy - kRingRadius, kRingRadius * 2, kRingRadius * 2),
                      static_cast<int>(-90.0 * 16),      // start at 12 o'clock
                      static_cast<int>(sweep_deg * 16)); // clockwise sweep
        }
    }

    // ── Digit (IBM Plex Mono 52px weight-500 amber tabular-nums centered) ────
    {
        QFont font;
        font.setFamily(QStringLiteral("IBM Plex Mono"));
        font.setPixelSize(static_cast<int>(kFontPx));
        font.setWeight(QFont::Medium); // ~500
        font.setStyleHint(QFont::Monospace);
        p.setFont(font);
        p.setPen(kAmber);

        const QString digit = QString::number((remaining_seconds_ > 0) ? remaining_seconds_ : 1);
        const QRect text_rect(0, 0, kCanvasSize, kCanvasSize);
        p.drawText(text_rect, Qt::AlignCenter, digit);
    }
}

QSize CountdownOverlayWindow::sizeHint() const {
    return QSize(kCanvasSize, kCanvasSize);
}

QSize CountdownOverlayWindow::minimumSizeHint() const {
    return sizeHint();
}

void CountdownOverlayWindow::applyExclusion() {
    exclusion_attempted_ = true;
    excluded_ = false;

#if defined(Q_OS_WIN)
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;
    const BOOL ok = ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    excluded_ = (ok != FALSE);
#else
    // Non-Windows: no capture exclusion supported; keep hidden.
    excluded_ = false;
#endif
}

void CountdownOverlayWindow::updatePosition() {
    const QSize hint = sizeHint();
    resize(hint);

    QRect target_rect = monitor_rect_;
    if (target_rect.isNull() || target_rect.isEmpty()) {
        const QScreen* primary = QGuiApplication::primaryScreen();
        if (primary)
            target_rect = primary->geometry();
    }

    if (target_rect.isNull() || target_rect.isEmpty()) {
        // Last resort: center near (0,0).
        move(-hint.width() / 2, -hint.height() / 2);
        return;
    }

    // Center on the recorded monitor.
    const int x = target_rect.left() + (target_rect.width() - hint.width()) / 2;
    const int y = target_rect.top() + (target_rect.height() - hint.height()) / 2;
    move(x, y);
}

} // namespace exosnap::ui::overlay
