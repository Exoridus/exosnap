#pragma once
#include <QSlider>
#include <QVector>

namespace exosnap::ui::widgets {

// S3 — ExoSlider: a QSlider subclass that paints gradient groove + tick marks.
//
// Tick positions are set via addTick(value) and rendered as short vertical lines
// below the groove at the configured slider positions.  A "default" marker at
// defaultValue() is painted more prominently (accent colour) so the user can
// always see the reset point.
//
// Usage:
//   auto* s = new ExoSlider(Qt::Horizontal, parent);
//   s->setRange(-12, 12);
//   s->setDefaultValue(0);           // 0 dB = unity gain
//   s->setTickValues({-12, -6, 0, 6, 12});
//
// The widget paints ticks in its own paintEvent after delegating to the style
// so it works with any platform QStyle (Windows Vista / Fusion) without calling
// QSlider::setTickPosition (which also draws native ticks we don't want).
class ExoSlider : public QSlider {
    Q_OBJECT

  public:
    explicit ExoSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

    // Set the "default" value that gets a prominent accent marker.
    void setDefaultValue(int value);
    [[nodiscard]] int defaultValue() const noexcept {
        return default_value_;
    }

    // Configure tick mark positions (in slider value units).
    void setTickValues(const QVector<int>& values);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    int default_value_ = 0;
    QVector<int> tick_values_;

    // Map a slider value to an x-coordinate within the groove rect.
    [[nodiscard]] double valueToX(const QRect& groove_rect, int value) const noexcept;
};

} // namespace exosnap::ui::widgets
