#include "CornerCaptureButton.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

CornerCaptureButton::CornerCaptureButton(QWidget* parent) : QAbstractButton(parent) {
    setFixedSize(40, 40);
    setCursor(Qt::PointingHandCursor);
    // icon_color_ default: invalid QColor -> falls back to kText0 in paintEvent
}

void CornerCaptureButton::setIcon(const QString& lucide_name) {
    icon_name_ = lucide_name;
    update();
}

void CornerCaptureButton::setIconColor(const QColor& color) {
    icon_color_ = color;
    update();
}

void CornerCaptureButton::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = rect();

    // Determine state colors
    QColor bg;
    QColor border;

    if (isDown()) {
        bg = QColor(QString::fromLatin1(ExoSnapPalette::kAccent));
        border = QColor(QString::fromLatin1(ExoSnapPalette::kAccent));
    } else if (underMouse() && !isDown()) {
        bg = QColor(QStringLiteral("#161618"));
        bg.setAlpha(234); // 0.92 * 255
        border = QColor(QString::fromLatin1(ExoSnapPalette::kLine3));
    } else {
        bg = QColor(QStringLiteral("#0E0E10"));
        bg.setAlpha(184); // 0.72 * 255
        border = QColor(QString::fromLatin1(ExoSnapPalette::kLine2));
    }

    if (!isEnabled()) {
        bg.setAlphaF(bg.alphaF() * 0.4);
        border.setAlphaF(border.alphaF() * 0.4);
    }

    // Draw circle background
    p.setPen(QPen(border, 1.0));
    p.setBrush(bg);
    p.drawEllipse(r.adjusted(0.5, 0.5, -0.5, -0.5));

    // Determine icon color
    QString icon_color_str;
    if (!isEnabled()) {
        icon_color_str = QString::fromLatin1(ExoSnapPalette::kText3);
    } else if (isDown()) {
        icon_color_str = QString::fromLatin1(ExoSnapPalette::kAccentInk);
    } else if (icon_color_.isValid()) {
        icon_color_str = icon_color_.name(QColor::HexArgb);
    } else {
        icon_color_str = QString::fromLatin1(ExoSnapPalette::kText0);
    }

    const qreal dpr = devicePixelRatioF();
    constexpr int kIconSize = 17;
    const QPixmap px = lucidePixmap(icon_name_, icon_color_str, kIconSize, dpr);

    // Center the icon
    const int x = (width() - kIconSize) / 2;
    const int y = (height() - kIconSize) / 2;
    p.drawPixmap(x, y, px);
}

} // namespace exosnap::ui::widgets
