#include "CameraPreview.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace exosnap::ui::widgets {

CameraPreview::CameraPreview(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("webcamCameraPreview"));
    setMinimumHeight(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

QSize CameraPreview::sizeHint() const {
    return QSize(480, 270);
}

void CameraPreview::setFrame(QImage frame) {
    frame_ = std::move(frame);
    update();
}

void CameraPreview::clearFrame() {
    if (frame_.isNull())
        return;
    frame_ = QImage{};
    update();
}

void CameraPreview::setPlaceholderText(const QString& text) {
    if (placeholder_ == text)
        return;
    placeholder_ = text;
    if (frame_.isNull())
        update();
}

void CameraPreview::setMirror(bool mirror) {
    if (mirror_ == mirror)
        return;
    mirror_ = mirror;
    if (!frame_.isNull())
        update();
}

void CameraPreview::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frame_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);

    QLinearGradient bg_grad(frame_rect.topLeft(), frame_rect.bottomRight());
    bg_grad.setColorAt(0.0, QColor("#181612"));
    bg_grad.setColorAt(1.0, QColor("#0e0d0b"));
    painter.setBrush(bg_grad);
    painter.setPen(QPen(QColor("#353330"), 1.0));
    painter.drawRoundedRect(frame_rect, 5.0, 5.0);

    if (!frame_.isNull()) {
        painter.save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(frame_rect, 5.0, 5.0);
        painter.setClipPath(clip_path);

        const double sx = static_cast<double>(width()) / frame_.width();
        const double sy = static_cast<double>(height()) / frame_.height();
        const double s = std::min(sx, sy);
        const int dw = static_cast<int>(frame_.width() * s);
        const int dh = static_cast<int>(frame_.height() * s);
        const int dx = (width() - dw) / 2;
        const int dy = (height() - dh) / 2;
        const QRect draw_rect(dx, dy, dw, dh);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // Real horizontal mirror about the draw rect's vertical centre (no vertical flip).
        if (mirror_) {
            painter.translate(draw_rect.center());
            painter.scale(-1.0, 1.0);
            painter.translate(-draw_rect.center());
        }
        painter.drawImage(draw_rect, frame_);
        painter.restore();
        return;
    }

    if (!placeholder_.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 120));
        const QRectF text_rect = frame_rect.adjusted(16, 16, -16, -16);
        painter.drawText(text_rect, Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
    }
}

} // namespace exosnap::ui::widgets
