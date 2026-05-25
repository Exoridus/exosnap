#include "PreviewSurface.h"

#include "StatusPill.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace exosnap::ui::widgets {

PreviewSurface::PreviewSurface(QWidget* parent) : QWidget(parent) {
    setObjectName("previewSurface");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    top_row_ = new QWidget(this);
    top_row_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* top_layout = new QHBoxLayout(top_row_);
    top_layout->setContentsMargins(0, 0, 0, 0);
    top_layout->setSpacing(8);

    status_pill_ = new StatusPill(top_row_);
    status_pill_->setTone(StatusPill::Tone::Ready);
    status_pill_->setText("READY");

    top_meta_label_ = new QLabel("NO TARGET", top_row_);
    top_meta_label_->setProperty("labelRole", "previewMeta");

    top_layout->addWidget(status_pill_, 0, Qt::AlignVCenter);
    top_layout->addWidget(top_meta_label_, 0, Qt::AlignVCenter);
    top_layout->addStretch(1);

    center_box_ = new QWidget(this);
    center_box_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* center_layout = new QVBoxLayout(center_box_);
    center_layout->setContentsMargins(0, 0, 0, 0);
    center_layout->setSpacing(6);
    center_layout->setAlignment(Qt::AlignCenter);

    center_title_label_ = new QLabel("SELECTED TARGET", center_box_);
    center_title_label_->setProperty("labelRole", "previewCenterTitle");
    center_title_label_->setAlignment(Qt::AlignCenter);
    center_subtitle_label_ = new QLabel("Preview not live in this alpha", center_box_);
    center_subtitle_label_->setProperty("labelRole", "previewCenterSubtitle");
    center_subtitle_label_->setAlignment(Qt::AlignCenter);

    center_layout->addWidget(center_title_label_, 0, Qt::AlignCenter);
    center_layout->addWidget(center_subtitle_label_, 0, Qt::AlignCenter);

    bottom_row_ = new QWidget(this);
    bottom_row_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* bottom_layout = new QHBoxLayout(bottom_row_);
    bottom_layout->setContentsMargins(0, 0, 0, 0);
    bottom_layout->setSpacing(8);

    bottom_left_label_ = new QLabel(QString(), bottom_row_);
    bottom_left_label_->setProperty("labelRole", "previewBottomText");
    bottom_left_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    bottom_left_label_->setVisible(false);

    bottom_right_label_ = new QLabel("AV1 · CQ 24", bottom_row_);
    bottom_right_label_->setProperty("labelRole", "previewBottomAccent");
    bottom_right_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    bottom_layout->addWidget(bottom_left_label_, 1);
    bottom_layout->addWidget(bottom_right_label_, 0);
}

bool PreviewSurface::hasHeightForWidth() const {
    return true;
}

int PreviewSurface::heightForWidth(int width) const {
    return static_cast<int>(static_cast<double>(width) * 9.0 / 16.0);
}

QSize PreviewSurface::sizeHint() const {
    return {720, 405};
}

QSize PreviewSurface::minimumSizeHint() const {
    return {320, 200};
}

void PreviewSurface::setRecording(bool recording) {
    if (recording_ == recording)
        return;
    recording_ = recording;
    center_box_->setVisible(current_frame_.isNull());
    update();
}

void PreviewSurface::setLiveFrame(QImage frame) {
    current_frame_ = std::move(frame);
    center_box_->setVisible(current_frame_.isNull());
    update();
}

bool PreviewSurface::isRecording() const noexcept {
    return recording_;
}

void PreviewSurface::setStatusText(const QString& text) {
    status_pill_->setText(text);
}

void PreviewSurface::setTopMetaText(const QString& text) {
    top_meta_label_->setText(text);
}

void PreviewSurface::setCenterTitle(const QString& text) {
    center_title_label_->setText(text);
}

void PreviewSurface::setCenterSubtitle(const QString& text) {
    center_subtitle_label_->setText(text);
}

void PreviewSurface::setBottomLeftText(const QString& text) {
    bottom_left_label_->setText(text);
    bottom_left_label_->setVisible(!text.trimmed().isEmpty());
}

void PreviewSurface::setBottomRightText(const QString& text) {
    bottom_right_label_->setText(text);
}

StatusPill* PreviewSurface::statusPill() const noexcept {
    return status_pill_;
}

// ---------------------------------------------------------------------------
// Webcam overlay helpers
// ---------------------------------------------------------------------------

void PreviewSurface::setWebcamFrame(QImage frame) {
    webcam_frame_ = std::move(frame);
    if (webcam_enabled_)
        update();
}

void PreviewSurface::setWebcamOverlayEnabled(bool enabled) {
    webcam_enabled_ = enabled;
    setMouseTracking(enabled);
    update();
}

void PreviewSurface::setWebcamOverlayRect(QRectF rect_norm) {
    webcam_rect_norm_ = rect_norm;
    update();
}

// Returns the webcam overlay rect in widget pixel coordinates.
QRectF PreviewSurface::webcamPixelRect() const {
    const QRectF fr = rect();
    return QRectF(fr.x() + webcam_rect_norm_.x() * fr.width(), fr.y() + webcam_rect_norm_.y() * fr.height(),
                  webcam_rect_norm_.width() * fr.width(), webcam_rect_norm_.height() * fr.height());
}

PreviewSurface::DragMode PreviewSurface::hitTestWebcam(QPointF pos) const {
    const QRectF r = webcamPixelRect();
    constexpr double kHandle = 10.0;
    const bool nearL = std::abs(pos.x() - r.left()) < kHandle;
    const bool nearR = std::abs(pos.x() - r.right()) < kHandle;
    const bool nearT = std::abs(pos.y() - r.top()) < kHandle;
    const bool nearB = std::abs(pos.y() - r.bottom()) < kHandle;
    if (nearT && nearL)
        return DragMode::ResizeTL;
    if (nearT && nearR)
        return DragMode::ResizeTR;
    if (nearB && nearL)
        return DragMode::ResizeBL;
    if (nearB && nearR)
        return DragMode::ResizeBR;
    if (r.contains(pos))
        return DragMode::Move;
    return DragMode::None;
}

void PreviewSurface::mousePressEvent(QMouseEvent* event) {
    if (!webcam_enabled_) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF pos = event->position();
    drag_mode_ = hitTestWebcam(pos);
    if (drag_mode_ != DragMode::None) {
        drag_origin_ = pos;
        drag_start_rect_ = webcam_rect_norm_;
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void PreviewSurface::mouseMoveEvent(QMouseEvent* event) {
    if (!webcam_enabled_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPointF pos = event->position();

    if (drag_mode_ == DragMode::None) {
        const DragMode hit = hitTestWebcam(pos);
        switch (hit) {
        case DragMode::ResizeTL:
        case DragMode::ResizeBR:
            setCursor(Qt::SizeFDiagCursor);
            break;
        case DragMode::ResizeTR:
        case DragMode::ResizeBL:
            setCursor(Qt::SizeBDiagCursor);
            break;
        case DragMode::Move:
            setCursor(Qt::SizeAllCursor);
            break;
        default:
            setCursor(Qt::ArrowCursor);
            break;
        }
        QWidget::mouseMoveEvent(event);
        return;
    }

    const double W = static_cast<double>(width());
    const double H = static_cast<double>(height());
    if (W < 1.0 || H < 1.0)
        return;

    const double dx = (pos.x() - drag_origin_.x()) / W;
    const double dy = (pos.y() - drag_origin_.y()) / H;
    QRectF r = drag_start_rect_;

    constexpr double kMinNorm = 0.05;
    switch (drag_mode_) {
    case DragMode::Move:
        r.translate(dx, dy);
        r.setLeft(std::clamp(r.left(), 0.0, 1.0 - r.width()));
        r.setTop(std::clamp(r.top(), 0.0, 1.0 - r.height()));
        break;
    case DragMode::ResizeBR:
        r.setRight(std::clamp(r.left() + kMinNorm, 0.0, 1.0));
        r.setBottom(std::clamp(r.top() + kMinNorm, 0.0, 1.0));
        r.setRight(std::clamp(drag_start_rect_.right() + dx, r.left() + kMinNorm, 1.0));
        r.setBottom(std::clamp(drag_start_rect_.bottom() + dy, r.top() + kMinNorm, 1.0));
        break;
    case DragMode::ResizeTL:
        r.setLeft(std::clamp(drag_start_rect_.left() + dx, 0.0, r.right() - kMinNorm));
        r.setTop(std::clamp(drag_start_rect_.top() + dy, 0.0, r.bottom() - kMinNorm));
        break;
    case DragMode::ResizeTR:
        r.setTop(std::clamp(drag_start_rect_.top() + dy, 0.0, r.bottom() - kMinNorm));
        r.setRight(std::clamp(drag_start_rect_.right() + dx, r.left() + kMinNorm, 1.0));
        break;
    case DragMode::ResizeBL:
        r.setLeft(std::clamp(drag_start_rect_.left() + dx, 0.0, r.right() - kMinNorm));
        r.setBottom(std::clamp(drag_start_rect_.bottom() + dy, r.top() + kMinNorm, 1.0));
        break;
    default:
        break;
    }

    webcam_rect_norm_ = r;
    update();
    event->accept();
}

void PreviewSurface::mouseReleaseEvent(QMouseEvent* event) {
    if (drag_mode_ != DragMode::None) {
        drag_mode_ = DragMode::None;
        emit webcamOverlayMoved(webcam_rect_norm_);
        event->accept();
    } else {
        QWidget::mouseReleaseEvent(event);
    }
}

// ---------------------------------------------------------------------------
// paintEvent
// ---------------------------------------------------------------------------

void PreviewSurface::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frame_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient bg_grad(frame_rect.topLeft(), frame_rect.bottomRight());
    bg_grad.setColorAt(0.0, QColor("#181612"));
    bg_grad.setColorAt(1.0, QColor("#0e0d0b"));
    painter.setBrush(bg_grad);
    painter.setPen(QPen(QColor("#3a342c"), 1.0));
    painter.drawRoundedRect(frame_rect, 5.0, 5.0);

    if (!current_frame_.isNull()) {
        painter.save();
        QPainterPath clipPath;
        clipPath.addRoundedRect(frame_rect, 5.0, 5.0);
        painter.setClipPath(clipPath);

        const double sx = static_cast<double>(width()) / current_frame_.width();
        const double sy = static_cast<double>(height()) / current_frame_.height();
        const double s = std::max(sx, sy);
        const int dw = static_cast<int>(current_frame_.width() * s);
        const int dh = static_cast<int>(current_frame_.height() * s);
        const int dx = (width() - dw) / 2;
        const int dy = (height() - dh) / 2;
        painter.drawImage(QRect(dx, dy, dw, dh), current_frame_);
        painter.restore();
    } else {
        painter.save();
        painter.setClipRect(rect().adjusted(1, 1, -1, -1));
        painter.setPen(QPen(QColor(255, 255, 255, 6), 1.0));
        for (int x = -height(); x < width() + height(); x += 12)
            painter.drawLine(x, height(), x + height(), 0);
        painter.restore();
    }

    if (recording_) {
        painter.save();
        painter.setClipRect(rect().adjusted(1, 1, -1, -1));
        painter.setPen(QPen(QColor(241, 180, 0, 10), 1.0));
        for (int y = 0; y < height(); y += 4)
            painter.drawLine(1, y, width() - 1, y);
        painter.restore();
    }

    // Webcam overlay
    if (webcam_enabled_) {
        const QRectF cam_rect = webcamPixelRect();
        if (!webcam_frame_.isNull()) {
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(cam_rect, webcam_frame_);
            painter.restore();
        } else {
            painter.save();
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.setPen(Qt::NoPen);
            painter.drawRect(cam_rect);
            painter.setPen(QPen(QColor(255, 255, 255, 100), 1.0));
            painter.setFont(QFont("Arial", 8));
            painter.drawText(cam_rect, Qt::AlignCenter, "CAM");
            painter.restore();
        }

        // Overlay border + resize handles
        painter.save();
        painter.setPen(QPen(QColor("#f1b400"), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(cam_rect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#f1b400"));
        constexpr double hs = 6.0;
        for (const QPointF& corner :
             {cam_rect.topLeft(), cam_rect.topRight(), cam_rect.bottomLeft(), cam_rect.bottomRight()}) {
            painter.drawRect(QRectF(corner.x() - hs / 2, corner.y() - hs / 2, hs, hs));
        }
        painter.restore();
    }

    painter.setPen(QPen(QColor("#f1b400"), 2.0));
    const int inset = 15;
    const int len = 20;
    painter.drawLine(inset, inset, inset + len, inset);
    painter.drawLine(inset, inset, inset, inset + len);
    painter.drawLine(width() - inset, inset, width() - inset - len, inset);
    painter.drawLine(width() - inset, inset, width() - inset, inset + len);
    painter.drawLine(inset, height() - inset, inset + len, height() - inset);
    painter.drawLine(inset, height() - inset, inset, height() - inset - len);
    painter.drawLine(width() - inset, height() - inset, width() - inset - len, height() - inset);
    painter.drawLine(width() - inset, height() - inset, width() - inset, height() - inset - len);
}

void PreviewSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    const int pad_x = 16;
    const int pad_top = 12;
    const int pad_bottom = 12;
    const int top_height = 24;
    const int bottom_height = 24;

    top_row_->setGeometry(pad_x, pad_top, width() - (pad_x * 2), top_height);
    center_box_->setGeometry(0, (height() - 90) / 2, width(), 90);
    bottom_row_->setGeometry(pad_x, height() - pad_bottom - bottom_height, width() - (pad_x * 2), bottom_height);
}

} // namespace exosnap::ui::widgets
