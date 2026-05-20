#include "BrandMarkWidget.h"

#include <QDebug>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

#include <cmath>

namespace exosnap::ui::brand {
namespace {

void drawFallbackMark(QPainter& painter, const QRectF& rect) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal w = rect.width();
    const qreal h = rect.height();
    const qreal pad = std::max<qreal>(1.0, std::floor(std::min(w, h) * 0.08));
    const QRectF b = rect.adjusted(pad, pad, -pad, -pad);

    QPainterPath e_shape;
    e_shape.moveTo(b.left(), b.top());
    e_shape.lineTo(b.right() * 0.84, b.top());
    e_shape.lineTo(b.right() * 0.66, b.top() + (b.height() * 0.18));
    e_shape.lineTo(b.left() + (b.width() * 0.22), b.top() + (b.height() * 0.18));
    e_shape.lineTo(b.left() + (b.width() * 0.22), b.top() + (b.height() * 0.40));
    e_shape.lineTo(b.left() + (b.width() * 0.50), b.center().y());
    e_shape.lineTo(b.left() + (b.width() * 0.22), b.top() + (b.height() * 0.62));
    e_shape.lineTo(b.left() + (b.width() * 0.22), b.bottom() - (b.height() * 0.18));
    e_shape.lineTo(b.right() * 0.66, b.bottom() - (b.height() * 0.18));
    e_shape.lineTo(b.right() * 0.84, b.bottom());
    e_shape.lineTo(b.left(), b.bottom());
    e_shape.closeSubpath();

    painter.fillPath(e_shape, QColor("#f1ece1"));

    QPainterPath caret;
    caret.moveTo(b.left(), b.top() + (b.height() * 0.40));
    caret.lineTo(b.left(), b.bottom() - (b.height() * 0.40));
    caret.lineTo(b.left() + (b.width() * 0.25), b.center().y());
    caret.closeSubpath();
    painter.fillPath(caret, QColor("#f1b400"));

    painter.restore();
}

} // namespace

BrandMarkWidget::BrandMarkWidget(QWidget* parent) : QWidget(parent) {
    static const QString kBrandLogoPath = QStringLiteral(":/brand/exosnap-logo.svg");
    if (!QFile::exists(kBrandLogoPath))
        qWarning().noquote() << "Brand logo resource missing:" << kBrandLogoPath;

    renderer_ = new QSvgRenderer(kBrandLogoPath, this);
    if (!renderer_->isValid())
        qWarning().noquote() << "Failed to load valid brand SVG renderer from:" << kBrandLogoPath;

    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kPreferredSize, kPreferredSize);
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
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (!renderer_ || !renderer_->isValid()) {
        drawFallbackMark(painter, rect());
        return;
    }

    QRectF target = rect();
    const QSize default_size = renderer_->defaultSize();
    if (default_size.isValid()) {
        QSizeF scaled = default_size;
        scaled.scale(target.size(), Qt::KeepAspectRatio);
        const QPointF top_left((target.width() - scaled.width()) * 0.5, (target.height() - scaled.height()) * 0.5);
        target = QRectF(top_left, scaled);
    }

    renderer_->render(&painter, target);
}

} // namespace exosnap::ui::brand
