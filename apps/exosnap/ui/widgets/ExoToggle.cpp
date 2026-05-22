#include "ExoToggle.h"

#include <QPainter>

namespace exosnap::ui::widgets {

ExoToggle::ExoToggle(QWidget* parent) : QAbstractButton(parent) {
    setObjectName("exoToggle");
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void ExoToggle::setOn(bool on) {
    setChecked(on);
}

bool ExoToggle::isOn() const noexcept {
    return isChecked();
}

QSize ExoToggle::sizeHint() const {
    return {34, 18};
}

QSize ExoToggle::minimumSizeHint() const {
    return sizeHint();
}

void ExoToggle::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    const bool enabled = isEnabled();
    const bool checked = isChecked();

    QColor track = QColor("#26221C");
    QColor thumb = QColor("#8A8070");
    if (!enabled) {
        track = QColor("#1A1714");
        thumb = QColor("#4A453E");
    } else if (checked) {
        track = QColor("#F1B400");
        thumb = QColor("#F1ECE1");
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(track_rect, 9.0, 9.0);

    constexpr qreal kThumbSize = 16.0;
    const qreal thumb_x = checked ? (track_rect.right() - kThumbSize - 1.0) : (track_rect.left() + 1.0);
    const qreal thumb_y = track_rect.top() + 1.0;
    const QRectF thumb_rect(thumb_x, thumb_y, kThumbSize, kThumbSize);
    painter.setBrush(thumb);
    painter.drawEllipse(thumb_rect);
}

} // namespace exosnap::ui::widgets
