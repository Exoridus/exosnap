#include "ExoCheckBox.h"

#include <QEnterEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"

namespace exosnap::ui::widgets {

ExoCheckBox::ExoCheckBox(const QString& text, QWidget* parent) : QAbstractButton(parent) {
    setObjectName("exoCheckBox");
    setCheckable(true);
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize ExoCheckBox::sizeHint() const {
    constexpr int kIndicatorSize = 18; // match canonical CheckTick 18px
    constexpr int kSpacing = 9;
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

    constexpr int kIndicatorSize = 18;
    constexpr int kTextSpacing = 9;

    const auto& theme = exosnap::ui::theme::ActiveTheme();
    const QColor ac(QString::fromUtf8(theme.ac));
    const QColor ac_ink(QString::fromUtf8(theme.ac_ink));
    const QColor ink(QString::fromUtf8(theme.ink));
    const QColor dim(QString::fromUtf8(theme.dim));
    // line2 is a rgba string; parse it via the theme helper
    const QColor line2 = exosnap::ui::theme::ParseThemeColor(theme.line2);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int indicator_y = (height() - kIndicatorSize) / 2;
    const QRectF box_rect(0.5, static_cast<qreal>(indicator_y) + 0.5, kIndicatorSize - 1.0, kIndicatorSize - 1.0);

    QColor fill;
    QColor border;
    if (!isEnabled()) {
        fill = Qt::transparent;
        border = line2;
        border.setAlphaF(border.alphaF() * 0.5);
    } else if (isChecked()) {
        fill = ac;
        border = ac;
    } else if (hovered_) {
        fill = Qt::transparent;
        border = ac;
    } else {
        fill = Qt::transparent;
        border = line2;
    }

    painter.setPen(QPen(border, 1.5));
    painter.setBrush(fill);
    painter.drawRoundedRect(box_rect, 5.0, 5.0); // radius-sm per design (18px box → 5px corner)

    if (isChecked()) {
        // CheckTick geometry: d="M4.4 9.3 L7.5 12.4 L13.6 5.7" (18×18 viewBox)
        QPainterPath path;
        const qreal ox = 0.0;
        const qreal oy = static_cast<qreal>(indicator_y);
        path.moveTo(ox + 4.4, oy + 9.3);
        path.lineTo(ox + 7.5, oy + 12.4);
        path.lineTo(ox + 13.6, oy + 5.7);
        painter.setPen(QPen(ac_ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    if (!text().isEmpty()) {
        painter.setPen(isEnabled() ? ink : dim);
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
