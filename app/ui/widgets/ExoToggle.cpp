#include "ExoToggle.h"

#include <QPainter>

#include "../theme/ExoSnapPalette.h"

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

    // #03: on-state track = Studio Mint accent; knob = accent-ink dark on mint.
    // Off-state: neutral dark track, muted knob. Disabled: dimmed.
    QColor track = QColor(255, 255, 255, static_cast<int>(0.10 * 255)); // rgba(255,255,255,0.10)
    QColor thumb = QColor("#65656A");                                   // mut (text3)
    if (!enabled) {
        track = QColor(255, 255, 255, static_cast<int>(0.05 * 255));
        thumb = QColor("#3A3A3F");
    } else if (checked) {
        track = QColor(theme::ExoSnapPalette::kAccent);    // Studio Mint
        thumb = QColor(theme::ExoSnapPalette::kAccentInk); // dark ink on accent fill
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
