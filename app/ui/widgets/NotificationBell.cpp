#include "NotificationBell.h"

#include <QEnterEvent>
#include <QFont>
#include <QPainter>
#include <QRectF>

#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

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

void NotificationBell::updateIcon() {
    const QString color = (unread_count_ > 0 || hovered_) ? QString::fromLatin1(ExoSnapPalette::kText0)
                                                          : QString::fromLatin1(ExoSnapPalette::kText2);
    const qreal dpr = devicePixelRatioF();
    setIcon(lucideIcon(QStringLiteral("bell"), color, 17, dpr));
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

    // Badge background (kWarn)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QString::fromLatin1(ExoSnapPalette::kWarn)));
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
