#include "CountdownSelect.h"

namespace exosnap::ui::widgets {

CountdownSelect::CountdownSelect(QWidget* parent) : QComboBox(parent) {
    setObjectName(QStringLiteral("recordCountdownSelect"));
    addItem(QStringLiteral("Off"));
    addItem(QStringLiteral("3s"));
    addItem(QStringLiteral("5s"));
    addItem(QStringLiteral("10s"));
    setCurrentIndex(0);
    // Honest state: recording delay is not yet wired into the capture path.
    setEnabled(false);
    setToolTip(QStringLiteral("Recording delay is planned — not yet wired into capture."));
    setFocusPolicy(Qt::NoFocus);
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
}

} // namespace exosnap::ui::widgets
