#include "ExoToggle.h"

#include <QPainter>

#include "../theme/ExoSnapTheme.h"

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
    // Off-state: ink-tinted track so it is visible on both dark and light themes
    //   (ink ≈ white on dark → same as the old hard-white; ink ≈ dark on light → visible on paper).
    // Disabled: same tint at half opacity.
    const auto& t = exosnap::ui::theme::ActiveTheme();
    const QColor ink(QString::fromUtf8(t.ink));
    QColor track = QColor::fromRgba(qRgba(ink.red(), ink.green(), ink.blue(), static_cast<int>(0.10 * 255)));
    QColor thumb = QColor("#65656A"); // mut (text3)
    if (!enabled) {
        track = QColor::fromRgba(qRgba(ink.red(), ink.green(), ink.blue(), static_cast<int>(0.05 * 255)));
        thumb = QColor("#3A3A3F");
    } else if (checked) {
        track = QColor(QString::fromUtf8(t.ac));     // primary accent
        thumb = QColor(QString::fromUtf8(t.ac_ink)); // dark ink on accent fill
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
