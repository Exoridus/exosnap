#include "CornerCaptureButton.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

CornerCaptureButton::CornerCaptureButton(QWidget* parent) : QAbstractButton(parent) {
    setFixedSize(40, 40);
    setCursor(Qt::PointingHandCursor);
    // icon_color_ default: invalid QColor -> falls back to ink in paintEvent
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
    const auto& t = exosnap::ui::theme::ActiveTheme();
    const QColor ac(QString::fromUtf8(t.ac));

    // Determine state colors
    QColor bg;
    QColor border;

    if (isDown()) {
        bg = ac;
        border = ac;
    } else if (underMouse() && !isDown()) {
        bg = QColor(QString::fromUtf8(t.surf));
        bg.setAlphaF(0.92f);
        border = exosnap::ui::theme::ParseThemeColor(t.line2);
        // line3 is derived: for dark rgba(255,255,255,0.20), for light rgba(ink,0.24)
        if (t.line3_override != nullptr) {
            border = exosnap::ui::theme::ParseThemeColor(t.line3_override);
        } else {
            border = QColor(QString::fromUtf8(t.ink));
            border.setAlphaF(0.24f);
        }
    } else {
        bg = QColor(QString::fromUtf8(t.bg));
        bg.setAlphaF(0.72f);
        border = exosnap::ui::theme::ParseThemeColor(t.line2);
    }

    if (!isEnabled()) {
        bg.setAlphaF(bg.alphaF() * 0.4f);
        border.setAlphaF(border.alphaF() * 0.4f);
    }

    // Draw circle background
    p.setPen(QPen(border, 1.0));
    p.setBrush(bg);
    p.drawEllipse(r.adjusted(0.5, 0.5, -0.5, -0.5));

    // Determine icon color
    QString icon_color_str;
    if (!isEnabled()) {
        icon_color_str = QString::fromUtf8(t.dim);
    } else if (isDown()) {
        icon_color_str = QString::fromUtf8(t.ac_ink);
    } else if (icon_color_.isValid()) {
        icon_color_str = icon_color_.name(QColor::HexArgb);
    } else {
        icon_color_str = QString::fromUtf8(t.ink);
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
