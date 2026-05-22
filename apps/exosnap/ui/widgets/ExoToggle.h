#pragma once

#include <QAbstractButton>

namespace exosnap::ui::widgets {

class ExoToggle : public QAbstractButton {
    Q_OBJECT
  public:
    explicit ExoToggle(QWidget* parent = nullptr);

    void setOn(bool on);
    [[nodiscard]] bool isOn() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace exosnap::ui::widgets
