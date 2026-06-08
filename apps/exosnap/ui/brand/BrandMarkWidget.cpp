#include "BrandMarkWidget.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QPointF>

#include <algorithm>

#include "../theme/ExoSnapPalette.h"

namespace exosnap::ui::brand {

BrandMarkWidget::BrandMarkWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kPreferredSize, kPreferredSize);
}

void BrandMarkWidget::setRecording(bool recording) {
    if (recording_ == recording)
        return;
    recording_ = recording;
    update();
}

QSize BrandMarkWidget::sizeHint() const {
    return QSize(kPreferredSize, kPreferredSize);
}

QSize BrandMarkWidget::minimumSizeHint() const {
    return sizeHint();
}

void BrandMarkWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw on the prototype's 32x32 design grid, scaled and centred into the widget rect so the
    // stroke weights track the prototype regardless of widget size.
    const qreal side = std::min(width(), height());
    if (side <= 0.0)
        return;
    const qreal scale = side / 32.0;
    painter.translate((width() - side) / 2.0, (height() - side) / 2.0);
    painter.scale(scale, scale);

    const QColor accent(theme::ExoSnapPalette::kAccent);
    const QColor recording(theme::ExoSnapPalette::kErr); // coral while recording
    const QColor inner_color = recording_ ? recording : accent;

    const QPointF center(16.0, 16.0);

    // Outer ring: accent, low opacity, thin stroke.
    QColor outer_color = accent;
    outer_color.setAlphaF(0.45f);
    QPen outer_pen(outer_color);
    outer_pen.setWidthF(1.5);
    painter.setPen(outer_pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, 14.5, 14.5);

    // Inner ring: accent (or coral while recording).
    QPen inner_pen(inner_color);
    inner_pen.setWidthF(1.6);
    painter.setPen(inner_pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, 6.2, 6.2);

    // Centre dot: filled accent (or coral while recording).
    painter.setPen(Qt::NoPen);
    painter.setBrush(inner_color);
    painter.drawEllipse(center, 2.4, 2.4);
}

} // namespace exosnap::ui::brand
