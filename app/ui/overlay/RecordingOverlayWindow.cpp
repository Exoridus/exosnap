#include "RecordingOverlayWindow.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>

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

// Design-system tokens (dark, compact pill — no QSS; painted directly)
constexpr QColor kBgColor{0x16, 0x16, 0x18, 230};       // ~90% opaque near-black
constexpr QColor kBorderColor{255, 255, 255, 28};       // subtle white hairline
constexpr QColor kTextColor{0xF1, 0xF1, 0xEF, 255};     // text-primary
constexpr QColor kDotRecColor{0xE0, 0x78, 0x6C, 255};   // coral (kErr from palette)
constexpr QColor kDotPauseColor{0xE6, 0xC5, 0x7C, 255}; // amber-warm (kWarn)

constexpr int kPaddingH = 14;    // horizontal inner padding
constexpr int kPaddingV = 8;     // vertical inner padding
constexpr int kDotSize = 8;      // dot diameter
constexpr int kDotTextGap = 7;   // gap between dot and text
constexpr int kRadius = 10;      // pill corner radius
constexpr int kMarginRight = 20; // distance from right edge of monitor
constexpr int kMarginTop = 20;   // distance from top edge of monitor

constexpr int kBlinkIntervalMs = 600; // blink period for paused dot

} // namespace

RecordingOverlayWindow::RecordingOverlayWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                          Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);

    // Blink timer for the paused state dot.
    blink_timer_ = new QTimer(this);
    blink_timer_->setInterval(kBlinkIntervalMs);
    connect(blink_timer_, &QTimer::timeout, this, [this]() {
        blink_on_ = !blink_on_;
        update();
    });
}

void RecordingOverlayWindow::showRecording(const QString& elapsed_text) {
    state_ = OverlayState::Recording;
    elapsed_text_ = elapsed_text;
    blink_timer_->stop();
    blink_on_ = true;

    if (!exclusion_attempted_) {
        applyExclusion();
    }

    if (!excluded_) {
        // Exclusion failed — refuse to show; overlay must not contaminate recording.
        return;
    }

    updatePosition();
    applyState();
    update();
    show();
    raise();
}

void RecordingOverlayWindow::showPaused(const QString& elapsed_text) {
    state_ = OverlayState::Paused;
    elapsed_text_ = elapsed_text;

    if (!exclusion_attempted_) {
        applyExclusion();
    }

    if (!excluded_) {
        return;
    }

    if (!blink_timer_->isActive())
        blink_timer_->start();
    blink_on_ = true;

    updatePosition();
    applyState();
    update();
    show();
    raise();
}

void RecordingOverlayWindow::updateElapsed(const QString& elapsed_text) {
    elapsed_text_ = elapsed_text;
    if (isVisible())
        update();
}

void RecordingOverlayWindow::hideOverlay() {
    blink_timer_->stop();
    hide();
}

void RecordingOverlayWindow::setMonitorGeometry(const QRect& monitor_rect) {
    monitor_rect_ = monitor_rect;
    if (isVisible())
        updatePosition();
}

bool RecordingOverlayWindow::isExcluded() const noexcept {
    return excluded_;
}

RecordingOverlayWindow::OverlayState RecordingOverlayWindow::overlayState() const noexcept {
    return state_;
}

const QString& RecordingOverlayWindow::elapsedText() const noexcept {
    return elapsed_text_;
}

void RecordingOverlayWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Reapply exclusion if the window handle was just created (first show).
    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            hide();
        }
    }
}

void RecordingOverlayWindow::paintEvent(QPaintEvent* /*event*/) {
    const QRect rect = this->rect();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background pill
    QPainterPath path;
    path.addRoundedRect(rect, kRadius, kRadius);
    p.fillPath(path, kBgColor);

    // Border hairline
    QPen border_pen(kBorderColor);
    border_pen.setWidthF(1.0);
    p.setPen(border_pen);
    p.drawPath(path);

    // Layout: [pad] [dot] [gap] [text] [pad]
    const int dot_cx = kPaddingH + kDotSize / 2;
    const int dot_cy = rect.height() / 2;

    // Dot
    if (blink_on_) {
        const QColor dot_color = (state_ == OverlayState::Recording) ? kDotRecColor : kDotPauseColor;
        p.setPen(Qt::NoPen);
        p.setBrush(dot_color);
        p.drawEllipse(QPoint(dot_cx, dot_cy), kDotSize / 2, kDotSize / 2);
    }

    // Label: "REC  00:00:00" or "PAUSED  00:00:00"
    const QString prefix = (state_ == OverlayState::Recording) ? QStringLiteral("REC") : QStringLiteral("PAUSED");
    const QString label = elapsed_text_.isEmpty() ? prefix : (prefix + QStringLiteral("  ") + elapsed_text_);

    QFont font = p.font();
    font.setFamily(QStringLiteral("JetBrains Mono"));
    font.setPointSizeF(10.0);
    font.setWeight(QFont::Medium);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    p.setFont(font);
    p.setPen(kTextColor);

    const int text_x = kPaddingH + kDotSize + kDotTextGap;
    const QRect text_rect(text_x, 0, rect.width() - text_x - kPaddingH, rect.height());
    p.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, label);
}

QSize RecordingOverlayWindow::minimumSizeHint() const {
    return sizeHint();
}

QSize RecordingOverlayWindow::sizeHint() const {
    // Measure text width using a temporary font metric.
    QFont font;
    font.setFamily(QStringLiteral("JetBrains Mono"));
    font.setPointSizeF(10.0);
    font.setWeight(QFont::Medium);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);

    const QString label = QStringLiteral("REC  00:00:00");
    const QFontMetrics fm(font);
    const int text_width = fm.horizontalAdvance(label);
    const int w = kPaddingH + kDotSize + kDotTextGap + text_width + kPaddingH;
    const int h = fm.height() + kPaddingV * 2;
    return QSize(w, h);
}

void RecordingOverlayWindow::applyExclusion() {
    exclusion_attempted_ = true;
    excluded_ = false;

#if defined(Q_OS_WIN)
    // Ensure the window handle exists. On first call this may not be created yet —
    // winId() forces creation.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    // WDA_EXCLUDEFROMCAPTURE (0x11) requires Windows 10 2004+.
    // SetWindowDisplayAffinity is available from Windows 8 but only
    // WDA_EXCLUDEFROMCAPTURE prevents the window from appearing in captures.
    const BOOL ok = ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    excluded_ = (ok != FALSE);
#else
    // Non-Windows: no capture exclusion supported; keep hidden.
    excluded_ = false;
#endif
}

void RecordingOverlayWindow::updatePosition() {
    const QSize hint = sizeHint();
    resize(hint);

    QRect target_rect = monitor_rect_;
    if (target_rect.isNull() || target_rect.isEmpty()) {
        // Fall back to the primary screen.
        const QScreen* primary = QGuiApplication::primaryScreen();
        if (primary)
            target_rect = primary->geometry();
    }

    if (target_rect.isNull() || target_rect.isEmpty()) {
        // Last resort: use (0,0) corner.
        move(kMarginRight, kMarginTop);
        return;
    }

    const int x = target_rect.right() - hint.width() - kMarginRight;
    const int y = target_rect.top() + kMarginTop;
    move(x, y);
}

void RecordingOverlayWindow::applyState() {
    // Nothing extra needed — paintEvent handles state-driven rendering.
}

} // namespace exosnap::ui::overlay
