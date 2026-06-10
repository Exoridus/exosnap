#pragma once

#include <QComboBox>

namespace exosnap::ui::widgets {

// Compact recording-delay selector for the Record transport dock (Off / 3s / 5s
// / 10s). The selected value is session-scoped in this slice; callers own any
// future persistence policy.
class CountdownSelect : public QComboBox {
    Q_OBJECT
  public:
    explicit CountdownSelect(QWidget* parent = nullptr);

    [[nodiscard]] int selectedSeconds() const;
    void setSelectedSeconds(int seconds);
    void setInteractive(bool interactive);

  signals:
    void selectedSecondsChanged(int seconds);
};

} // namespace exosnap::ui::widgets
