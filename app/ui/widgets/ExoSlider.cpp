#include "ExoSlider.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <cmath>

#include "../theme/ExoSnapPalette.h"

namespace exosnap::ui::widgets {

using P = exosnap::ui::theme::ExoSnapPalette;

ExoSlider::ExoSlider(Qt::Orientation orientation, QWidget* parent) : QSlider(orientation, parent) {
    // Do not use Qt's built-in tick rendering — we paint our own below the groove.
    setTickPosition(QSlider::NoTicks);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ExoSlider::setDefaultValue(int value) {
    default_value_ = value;
    update();
}

void ExoSlider::setTickValues(const QVector<int>& values) {
    tick_values_ = values;
    update();
}

double ExoSlider::valueToX(const QRect& groove_rect, int value) const noexcept {
    const int min_val = minimum();
    const int max_val = maximum();
    if (max_val == min_val)
        return static_cast<double>(groove_rect.center().x());
    const double ratio = static_cast<double>(value - min_val) / static_cast<double>(max_val - min_val);
    return groove_rect.left() + ratio * (groove_rect.width() - 1);
}

void ExoSlider::paintEvent(QPaintEvent* event) {
    // Delegate base rendering (gradient groove + handle are driven by QSS).
    QSlider::paintEvent(event);

    if (tick_values_.isEmpty())
        return;

    // Resolve groove geometry from the current style so tick positions align exactly.
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove_rect = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    if (groove_rect.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Ticks are drawn below the groove: 2 px gap then a short vertical line.
    const int tick_top = groove_rect.bottom() + 3;
    constexpr int kTickHeight = 5;
    constexpr int kDefaultTickHeight = 8;

    const QColor tick_color(P::kText3);
    const QColor default_color(P::kAccent);

    for (const int val : tick_values_) {
        if (val < minimum() || val > maximum())
            continue;
        const int x = static_cast<int>(std::round(valueToX(groove_rect, val)));
        const bool is_default = (val == default_value_);
        p.setPen(QPen(is_default ? default_color : tick_color, is_default ? 2 : 1));
        p.drawLine(x, tick_top, x, tick_top + (is_default ? kDefaultTickHeight : kTickHeight));
    }
}

} // namespace exosnap::ui::widgets
