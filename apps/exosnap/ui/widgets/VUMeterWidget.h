#pragma once

#include <QWidget>

namespace exosnap::ui::widgets {

class VUMeterWidget : public QWidget {
    Q_OBJECT
  public:
    explicit VUMeterWidget(QWidget* parent = nullptr);

    void setLevel(float level01);
    float level() const noexcept;

    void setSegmentCount(int segments);
    int segmentCount() const noexcept;

    void setActive(bool active);
    bool isActive() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    float level_ = 0.0F;
    int segments_ = 22;
    bool active_ = true;
};

} // namespace exosnap::ui::widgets
