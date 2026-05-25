#pragma once

#include <QObject>

class QComboBox;
class QEvent;
class QWheelEvent;

namespace exosnap::ui::widgets {

// Prevents accidental value changes while scrolling over unfocused combo boxes.
class ComboBoxWheelFilter final : public QObject {
  public:
    explicit ComboBoxWheelFilter(QObject* parent = nullptr);

    void installOn(QComboBox* combo);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void forwardWheelToScrollArea(QComboBox* combo, QWheelEvent* wheel_event) const;
};

} // namespace exosnap::ui::widgets
