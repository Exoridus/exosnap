#include "ExoCheckBox.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace exosnap::ui::widgets {

ExoCheckBox::ExoCheckBox(const QString& text, QWidget* parent) : QAbstractButton(parent) {
    setObjectName("exoCheckBox");
    setCheckable(true);
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize ExoCheckBox::sizeHint() const {
    constexpr int kIndicatorSize = 14;
    constexpr int kSpacing = 8;
    constexpr int kVerticalMargin = 2;

    QFontMetrics fm(font());
    const int text_width = fm.horizontalAdvance(text());
    const int text_height = fm.height();
    const int height = qMax(kIndicatorSize, text_height) + (kVerticalMargin * 2);
    const int width = kIndicatorSize + (text().isEmpty() ? 0 : (kSpacing + text_width));
    return {width, height};
}

QSize ExoCheckBox::minimumSizeHint() const {
    return sizeHint();
}

void ExoCheckBox::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    constexpr int kIndicatorSize = 14;
    constexpr int kTextSpacing = 8;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int indicator_y = (height() - kIndicatorSize) / 2;
    const QRectF box_rect(0.5, static_cast<qreal>(indicator_y) + 0.5, kIndicatorSize - 1.0, kIndicatorSize - 1.0);

    QColor fill = QColor("#26221C");
    QColor border = QColor("#3A342C");
    if (!isEnabled()) {
        fill = QColor("#1A1714");
        border = QColor("#3A342C");
    } else if (isChecked()) {
        fill = QColor("#F1B400");
        border = QColor("#F1B400");
    } else if (hovered_) {
        border = QColor(241, 180, 0, 102);
    }

    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(box_rect, 3.0, 3.0);

    if (isChecked()) {
        QPainterPath path;
        path.moveTo(3.5, indicator_y + 7.8);
        path.lineTo(6.0, indicator_y + 10.2);
        path.lineTo(10.6, indicator_y + 4.2);
        painter.setPen(QPen(QColor("#0E0D0B"), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    if (!text().isEmpty()) {
        painter.setPen(isEnabled() ? QColor("#C7C0B1") : QColor("#6A6258"));
        const int text_x = kIndicatorSize + kTextSpacing;
        painter.drawText(QRect(text_x, 0, width() - text_x, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }
}

void ExoCheckBox::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void ExoCheckBox::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

} // namespace exosnap::ui::widgets
