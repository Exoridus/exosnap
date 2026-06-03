#include "AudioSourceToggle.h"

#include "../theme/ExoSnapPalette.h"

#include <QByteArray>
#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

namespace exosnap::ui::widgets {
namespace {

constexpr int kDiameter = 42;

// Lucide-style 24x24 stroke paths, matching the hybrid design icon set.
QByteArray iconPathFor(const QString& key) {
    if (key == QLatin1String("system"))
        return QByteArrayLiteral("M11 5L6 9H2v6h4l5 4V5zM15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13");
    if (key == QLatin1String("mic"))
        return QByteArrayLiteral("M12 3a3 3 0 0 0-3 3v5a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM5 11a7 7 0 0 0 14 0M12 18v3");
    if (key == QLatin1String("webcam"))
        return QByteArrayLiteral("M12 13a4 4 0 1 0 0-8 4 4 0 0 0 0 8zM3 20a9 9 0 0 1 18 0");
    if (key == QLatin1String("app"))
        return QByteArrayLiteral("M4 4h7v7H4zM13 4h7v7h-7zM13 13h7v7h-7zM4 13h7v7H4z");
    return {};
}

void paintIcon(QPainter& painter, const QString& key, const QRectF& bounds, const QColor& color) {
    const QByteArray path = iconPathFor(key);
    if (path.isEmpty())
        return;
    QByteArray svg;
    svg.reserve(256);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(color.name(QColor::HexRgb).toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='");
    svg.append(path);
    svg.append("'/></svg>");
    QSvgRenderer renderer(svg);
    renderer.render(&painter, bounds);
}

} // namespace

AudioSourceToggle::AudioSourceToggle(const QString& icon_key, const QString& source_key, QWidget* parent)
    : QAbstractButton(parent), icon_key_(icon_key), source_key_(source_key) {
    setObjectName(QStringLiteral("audioSourceToggle"));
    setProperty("sourceKey", source_key_);
    setProperty("toggleOn", false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kDiameter, kDiameter);
}

void AudioSourceToggle::setOn(bool on) {
    if (on_ == on)
        return;
    on_ = on;
    setProperty("toggleOn", on_);
    update();
}

void AudioSourceToggle::setInteractive(bool interactive) {
    interactive_ = interactive;
    setEnabled(interactive); // disabled => no clicked() signal
    setCursor(interactive ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

QSize AudioSourceToggle::sizeHint() const {
    return {kDiameter, kDiameter};
}

QSize AudioSourceToggle::minimumSizeHint() const {
    return {kDiameter, kDiameter};
}

void AudioSourceToggle::paintEvent(QPaintEvent* /*event*/) {
    using theme::ExoSnapPalette;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF circle = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);

    // ExoSnapPalette exposes accent-alpha roles as CSS rgba() strings (for QSS),
    // which QColor cannot parse — derive the alpha variants from the accent hex.
    const QColor accent(ExoSnapPalette::kAccent);

    QColor fill;
    QColor border;
    QColor icon;
    if (on_) {
        fill = accent;
        fill.setAlphaF(0.14f); // matches kAccentDim
        border = accent;
        border.setAlphaF(0.42f); // matches kAccentLine
        icon = accent;
    } else {
        fill = QColor(255, 255, 255, 13); // subtle raise, matches hybrid "off" pill
        border = QColor(Qt::transparent);
        icon = QColor(ExoSnapPalette::kText2);
    }

    painter.setPen(border.alpha() > 0 ? QPen(border, 1.0) : Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(circle);

    const QRectF icon_rect =
        circle.adjusted(circle.width() * 0.27, circle.height() * 0.27, -circle.width() * 0.27, -circle.height() * 0.27);
    paintIcon(painter, icon_key_, icon_rect, icon);
}

} // namespace exosnap::ui::widgets
