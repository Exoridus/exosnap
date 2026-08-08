#include "NotificationBell.h"

#include <QEnterEvent>
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
    setFixedSize(kSize, kSize);
    setIconSize(QSize(kIconSize, kIconSize));
    setCursor(Qt::PointingHandCursor);
    updateIcon();
}

void NotificationBell::setUnreadStatus(const QString& status) {
    if (status == unread_status_)
        return;
    unread_status_ = status;
    updateIcon();
    update();
}

QPoint NotificationBell::dotCenter() const {
    // The icon sits centred in the button; its top-right corner is where a badge
    // belongs. Pulled one pixel inwards on each axis so the dot overlaps the
    // glyph's outline slightly and reads as one object with it.
    const int icon_right = (width() + kIconSize) / 2;
    const int icon_top = (height() - kIconSize) / 2;
    return QPoint(icon_right - 1, icon_top + 1);
}

QColor NotificationBell::dotColor() const {
    const auto& t = exosnap::ui::theme::ActiveTheme();
    if (unread_status_ == QStringLiteral("error"))
        return QColor(QString::fromUtf8(t.error));
    if (unread_status_ == QStringLiteral("caution"))
        return QColor(QString::fromUtf8(t.caution));
    // "info", "success" and anything unrecognised: the neutral hint. Unread mail
    // is not a warning, and caution/error are reserved for the two rungs above.
    return QColor(QString::fromUtf8(t.ac));
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
    const QString color = (hasUnread() || hovered_ || hub_open_) ? QString::fromUtf8(t.ink) : QString::fromUtf8(t.mut);
    const qreal dpr = devicePixelRatioF();
    setIcon(exosnap::ui::theme::lucideIcon(QStringLiteral("bell"), color, kIconSize, dpr));
}

void NotificationBell::paintEvent(QPaintEvent* event) {
    QToolButton::paintEvent(event);

    if (!hasUnread())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Unread dot at the bell glyph's top-right corner, with a bg-coloured ring so
    // it stays legible where it overlaps the bell's own strokes (VG-12: separation
    // from the icon).
    constexpr qreal kRing = 2.0;
    p.setPen(QPen(QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().bg)), kRing));
    p.setBrush(dotColor());
    p.drawEllipse(QPointF(dotCenter()), kDotDiameter / 2.0, kDotDiameter / 2.0);
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
