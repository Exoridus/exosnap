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

    // Unread dot: top-right corner, with a bg-coloured ring so it stays legible
    // where it overlaps the bell's own strokes (VG-12: separation from the icon).
    // The ring is stroked outside the fill, so the dot is inset by its width to
    // keep the whole thing inside the widget.
    constexpr qreal kRing = 2.0;
    const qreal d = static_cast<qreal>(kDotDiameter);
    const qreal x = width() - d - kRing / 2.0;
    const qreal y = kRing / 2.0;

    p.setPen(QPen(QColor(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().bg)), kRing));
    p.setBrush(dotColor());
    p.drawEllipse(QRectF(x, y, d, d));
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
