#include "ExoSpinBox.h"

namespace exosnap::ui::widgets {

ExoSpinBox::ExoSpinBox(QWidget* parent) : QSpinBox(parent) {
    setProperty("widgetRole", "exoSpinBox");
    setMinimumSize(100, 32);
}

} // namespace exosnap::ui::widgets
