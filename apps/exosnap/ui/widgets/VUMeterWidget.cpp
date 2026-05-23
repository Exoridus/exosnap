#include "VUMeterWidget.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

namespace exosnap::ui::widgets {

VUMeterWidget::VUMeterWidget(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(14);
}

void VUMeterWidget::setLevel(float level01) {
    const float clamped = std::clamp(level01, 0.0F, 1.0F);
    if (qFuzzyCompare(level_, clamped))
        return;
    level_ = clamped;
    update();
}

float VUMeterWidget::level() const noexcept {
    return level_;
}

void VUMeterWidget::setSegmentCount(int segments) {
    if (segments < 4)
        segments = 4;
    if (segments_ == segments)
        return;
    segments_ = segments;
    update();
}

int VUMeterWidget::segmentCount() const noexcept {
    return segments_;
}

void VUMeterWidget::setActive(bool active) {
    if (active_ == active)
        return;
    active_ = active;
    update();
}

bool VUMeterWidget::isActive() const noexcept {
    return active_;
}

QSize VUMeterWidget::sizeHint() const {
    return {140, 14};
}

QSize VUMeterWidget::minimumSizeHint() const {
    return {72, 14};
}

void VUMeterWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int spacing = 2;
    const int width_for_segments = width() - (segments_ - 1) * spacing;
    const int segment_width = std::max(2, width_for_segments / segments_);
    const int lit_segments = static_cast<int>(level_ * static_cast<float>(segments_) + 0.5F);

    int x = 0;
    for (int i = 0; i < segments_; ++i) {
        const bool lit = active_ && (i < lit_segments);
        QColor color("#2a2620");
        if (lit) {
            const float ratio = static_cast<float>(i) / static_cast<float>(std::max(1, segments_ - 1));
            if (ratio >= 0.86F) {
                color = QColor("#e26a5a");
            } else if (ratio >= 0.62F) {
                color = QColor("#f1b400");
            } else {
                color = QColor("#74c08a");
            }
        }

        painter.fillRect(QRect(x, 1, segment_width, height() - 2), color);
        x += segment_width + spacing;
    }
}

} // namespace exosnap::ui::widgets
