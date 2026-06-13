#include "NotificationToastWindow.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QShowEvent>

#include "notifications/NotificationManager.h"

#if defined(Q_OS_WIN)
#include <windows.h>
// WDA_EXCLUDEFROMCAPTURE was introduced in Windows 10 2004 (build 19041).
#if !defined(WDA_EXCLUDEFROMCAPTURE)
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace exosnap::ui::overlay {

// VISUAL PLACEHOLDER — final anatomy from NOTIFY-DESIGN-R1
// All paint constants here are placeholders pending final design review.
namespace {

// Design-system tokens (match RecordingOverlayWindow)
constexpr QColor kBgColor{0x16, 0x16, 0x18, 230};       // ~90% opaque near-black
constexpr QColor kBorderColor{255, 255, 255, 28};       // subtle white hairline
constexpr QColor kTextPrimary{0xF1, 0xF1, 0xEF, 255};   // text-primary
constexpr QColor kTextSecondary{0xA0, 0xA0, 0x9E, 255}; // text-secondary

// Per-type dot colors — reuse coral/amber/status tokens from RecordingOverlayWindow
constexpr QColor kDotSaved{0x4A, 0xD2, 0x8E, 255};          // green (success)
constexpr QColor kDotLowStorage{0xE6, 0xC5, 0x7C, 255};     // amber-warm (kWarn)
constexpr QColor kDotUnexpectedStop{0xE0, 0x78, 0x6C, 255}; // coral (kErr)
constexpr QColor kDotRecovery{0x60, 0xA5, 0xE0, 255};       // blue-info

constexpr int kToastWidth = 280;
constexpr int kToastPadH = 12;
constexpr int kToastPadV = 9;
constexpr int kDotSize = 8;
constexpr int kDotGap = 8;
constexpr int kRadius = 8;
constexpr int kRowGap = 6; // gap between stacked toasts
constexpr int kMarginRight = 20;
constexpr int kMarginBottom = 20;

QColor dotColorForType(notifications::NotificationType type) noexcept {
    switch (type) {
    case notifications::NotificationType::Saved:
        return kDotSaved;
    case notifications::NotificationType::LowStorage:
        return kDotLowStorage;
    case notifications::NotificationType::UnexpectedStop:
        return kDotUnexpectedStop;
    case notifications::NotificationType::RecoveryAvailable:
        return kDotRecovery;
    }
    return kDotSaved;
}

// Height of a single toast cell given title+body text.
constexpr int kToastHeight = 52; // placeholder constant; final value from NOTIFY-DESIGN-R1

} // namespace

NotificationToastWindow::NotificationToastWindow(notifications::NotificationManager* manager, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                          Qt::NoDropShadowWindowHint),
      manager_(manager) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);

    if (manager_) {
        connect(manager_, &notifications::NotificationManager::visibleSetChanged, this,
                &NotificationToastWindow::onVisibleSetChanged);
    }
}

bool NotificationToastWindow::isExcluded() const noexcept {
    return excluded_;
}

QSize NotificationToastWindow::sizeHint() const {
    const int n = manager_ ? manager_->VisibleEvents().size() : 0;
    if (n == 0)
        return QSize(kToastWidth, 0);

    const int h = n * kToastHeight + (n - 1) * kRowGap;
    return QSize(kToastWidth, h);
}

QSize NotificationToastWindow::minimumSizeHint() const {
    return sizeHint();
}

void NotificationToastWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            hide();
        }
    }
}

void NotificationToastWindow::paintEvent(QPaintEvent* /*event*/) {
    if (!manager_)
        return;

    // VISUAL PLACEHOLDER — final anatomy from NOTIFY-DESIGN-R1
    const auto& events = manager_->VisibleEvents();
    if (events.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont title_font = p.font();
    title_font.setFamily(QStringLiteral("IBM Plex Mono"));
    title_font.setPointSizeF(9.5);
    title_font.setWeight(QFont::Medium);

    QFont body_font = p.font();
    body_font.setFamily(QStringLiteral("IBM Plex Mono"));
    body_font.setPointSizeF(8.5);
    body_font.setWeight(QFont::Normal);

    int y_offset = 0;
    for (const auto& event : events) {
        const QRect cell(0, y_offset, kToastWidth, kToastHeight);

        // Background pill
        QPainterPath path;
        path.addRoundedRect(cell, kRadius, kRadius);
        p.fillPath(path, kBgColor);

        // Border
        QPen border_pen(kBorderColor);
        border_pen.setWidthF(1.0);
        p.setPen(border_pen);
        p.drawPath(path);

        // Leading dot
        const QColor dot_color = dotColorForType(event.type);
        p.setPen(Qt::NoPen);
        p.setBrush(dot_color);
        const int dot_cx = kToastPadH + kDotSize / 2;
        const int dot_cy = cell.top() + kToastHeight / 2;
        p.drawEllipse(QPoint(dot_cx, dot_cy), kDotSize / 2, kDotSize / 2);

        const int text_x = kToastPadH + kDotSize + kDotGap;
        const int text_w = kToastWidth - text_x - kToastPadH;

        // Title line
        p.setFont(title_font);
        p.setPen(kTextPrimary);
        const QRect title_rect(text_x, cell.top() + kToastPadV, text_w, 20);
        p.drawText(title_rect, Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(event.title, Qt::ElideRight, text_w));

        // Body line
        p.setFont(body_font);
        p.setPen(kTextSecondary);
        const QRect body_rect(text_x, cell.top() + kToastPadV + 20, text_w, 18);
        p.drawText(body_rect, Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(event.body, Qt::ElideRight, text_w));

        y_offset += kToastHeight + kRowGap;
    }
}

void NotificationToastWindow::applyExclusion() {
    exclusion_attempted_ = true;
    excluded_ = false;

#if defined(Q_OS_WIN)
    // Ensure the HWND exists. winId() forces creation.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    // WDA_EXCLUDEFROMCAPTURE (0x11) requires Windows 10 2004+.
    // If this fails the window hides and stays hidden for the session — an
    // overlay that cannot guarantee capture exclusion must not be visible.
    const BOOL ok = ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    excluded_ = (ok != FALSE);
#else
    excluded_ = false;
#endif
}

void NotificationToastWindow::updatePosition() {
    const QSize hint = sizeHint();
    if (hint.height() <= 0) {
        hide();
        return;
    }

    resize(hint);

    // Anchor to the PRIMARY display (not the recorded monitor; notifications
    // are app-level events per ADR 0016).
    QRect screen_rect;
    const QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
        screen_rect = primary->geometry();

    if (screen_rect.isNull() || screen_rect.isEmpty()) {
        // Fallback: top-left origin
        move(kMarginRight, kMarginBottom);
        return;
    }

    const int x = screen_rect.right() - hint.width() - kMarginRight;
    const int y = screen_rect.bottom() - hint.height() - kMarginBottom;
    move(x, y);
}

void NotificationToastWindow::onVisibleSetChanged() {
    if (!excluded_ && exclusion_attempted_) {
        // Exclusion failed at startup; refuse to show.
        return;
    }

    const bool has_events = manager_ && !manager_->VisibleEvents().isEmpty();

    if (!has_events) {
        hide();
        return;
    }

    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            return; // hide and stay hidden
        }
    }

    updatePosition();
    update();
    show();
    raise();
}

} // namespace exosnap::ui::overlay
