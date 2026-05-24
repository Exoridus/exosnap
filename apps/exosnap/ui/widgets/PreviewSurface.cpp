#include "PreviewSurface.h"

#include "StatusPill.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

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
