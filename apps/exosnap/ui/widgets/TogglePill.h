#pragma once

#include <QPushButton>

namespace exosnap::ui::widgets {

class TogglePill : public QPushButton {
  public:
    explicit TogglePill(QWidget* parent = nullptr);

    void setOn(bool on);
    [[nodiscard]] bool isOn() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  private:
    void syncToggleState();
};

} // namespace exosnap::ui::widgets
