#pragma once

#include <QWidget>

class QPaintEvent;
class QSvgRenderer;

namespace exosnap::ui::brand {

class BrandMarkWidget : public QWidget {
  public:
    explicit BrandMarkWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    static constexpr int kPreferredSize = 34;
    QSvgRenderer* renderer_ = nullptr;
};

} // namespace exosnap::ui::brand
