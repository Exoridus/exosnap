#include "CountdownSelect.h"

#include <QIcon>
#include <QSize>

namespace exosnap::ui::widgets {

CountdownSelect::CountdownSelect(QWidget* parent) : QComboBox(parent) {
    setObjectName(QStringLiteral("recordCountdownSelect"));

    // #12: Leading 14px timer glyph, mut, 6px gap before value.
    const QIcon timer_icon(QStringLiteral(":/theme/icons/timer.svg"));
    if (!timer_icon.isNull()) {
        setIconSize(QSize(14, 14));
        addItem(timer_icon, QStringLiteral("Off"), 0);
        addItem(timer_icon, QStringLiteral("3s"), 3);
        addItem(timer_icon, QStringLiteral("5s"), 5);
        addItem(timer_icon, QStringLiteral("10s"), 10);
    } else {
        addItem(QStringLiteral("Off"), 0);
        addItem(QStringLiteral("3s"), 3);
        addItem(QStringLiteral("5s"), 5);
        addItem(QStringLiteral("10s"), 10);
    }
    setCurrentIndex(0);
    setEnabled(true);
    setToolTip(QStringLiteral("Delay before recording starts."));
    setFocusPolicy(Qt::NoFocus);
    // Compact fixed height to sit flush with other dock elements.
    setFixedHeight(32);

    connect(this, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { emit selectedSecondsChanged(selectedSeconds()); });
}

int CountdownSelect::selectedSeconds() const {
    return currentData().toInt();
}

void CountdownSelect::setSelectedSeconds(int seconds) {
    const int index = findData(seconds);
    if (index >= 0 && index != currentIndex()) {
        setCurrentIndex(index);
    }
}

void CountdownSelect::setInteractive(bool interactive) {
    setEnabled(interactive);
    setToolTip(interactive ? QStringLiteral("Delay before recording starts.")
                           : QStringLiteral("Countdown cannot change while recording controls are locked."));
}

} // namespace exosnap::ui::widgets
