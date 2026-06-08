#pragma once

#include <QComboBox>

namespace exosnap::ui::widgets {

// Compact recording-delay selector for the Record transport dock (Off / 3s / 5s
// / 10s). The recording-delay feature is not yet wired into the capture path, so
// the control is presented as planned/disabled and never fakes a countdown. It
// reserves the dock's secondary-action slot so the Ready layout matches the
// hybrid target geometry.
class CountdownSelect : public QComboBox {
    Q_OBJECT
  public:
    explicit CountdownSelect(QWidget* parent = nullptr);
};

} // namespace exosnap::ui::widgets
