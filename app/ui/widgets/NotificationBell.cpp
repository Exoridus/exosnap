#include "NotificationBell.h"

#include <QEnterEvent>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QStyle>

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

NotificationBell::NotificationBell(QWidget* parent) : QToolButton(parent) {
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(34, 34);
    setIconSize(QSize(17, 17));
    updateIcon();
}

void NotificationBell::setUnreadCount(int count) {
    if (count == unread_count_)
        return;
    unread_count_ = count;
    updateIcon();
    update();
}

void NotificationBell::setHubOpen(bool open) {
    if (hub_open_ == open)
        return;
    hub_open_ = open;
    setProperty("hubOpen", open);
    style()->unpolish(this);
    style()->polish(this);
    updateIcon();
}

void NotificationBell::updateIcon() {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    const QString color =
        (unread_count_ > 0 || hovered_ || hub_open_) ? QString::fromUtf8(t.ink) : QString::fromUtf8(t.mut);
    const qreal dpr = devicePixelRatioF();
    setIcon(exosnap::ui::theme::lucideIcon(QStringLiteral("bell"), color, 17, dpr));
}

void NotificationBell::paintEvent(QPaintEvent* event) {
    QToolButton::paintEvent(event);

    if (unread_count_ <= 0)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Badge oval: top-right corner, small rounded rect
    const int badge_w = (unread_count_ >= 10) ? 18 : 14;
    const int badge_h = 13;
    const int badge_x = width() - badge_w - 1;
    const int badge_y = 2;

    // Badge background (caution) + bg-colored outline ring (VG-12: visual separation from icon)
    const QColor badge_border_c(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().bg));
    p.setPen(QPen(badge_border_c, 1.5));
    p.setBrush(QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().caution)));
    p.drawRoundedRect(QRectF(badge_x, badge_y, badge_w, badge_h), 6.5, 6.5);

    // Badge text
    QFont f;
    f.setFamily(QStringLiteral("IBM Plex Mono"));
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(QStringLiteral("#1A1206")));
    p.drawText(QRect(badge_x, badge_y, badge_w, badge_h), Qt::AlignCenter,
               QString::number(unread_count_ > 99 ? 99 : unread_count_));
}

void NotificationBell::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    updateIcon();
    QToolButton::enterEvent(event);
}

void NotificationBell::leaveEvent(QEvent* event) {
    hovered_ = false;
    updateIcon();
    QToolButton::leaveEvent(event);
}

void NotificationBell::changeEvent(QEvent* event) {
    QToolButton::changeEvent(event);
    updateIcon();
}

} // namespace exosnap::ui::widgets
