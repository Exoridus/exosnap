#pragma once
#include <QSpinBox>

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Numeric input, styled like the suite-kit Spinbox mockup.
// Surface kBg3, 1px kLine2 border, accent focus-outline, stepper buttons.
class ExoSpinBox : public QSpinBox {
    Q_OBJECT
  public:
    explicit ExoSpinBox(QWidget* parent = nullptr);
    // Re-exports from QSpinBox for documentation clarity:
    // void setSuffix(const QString& suffix) — already inherited
    // void setRange(int min, int max)       — already inherited
    // void setValue(int v)                  — already inherited
    // int value() const                     — already inherited
    // signal valueChanged(int)              — already inherited
};

} // namespace exosnap::ui::widgets
