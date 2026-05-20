#include "TogglePill.h"

#include <QStyle>

namespace exosnap::ui::widgets {
namespace {

void restyle(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

TogglePill::TogglePill(QWidget* parent) : QPushButton(parent) {
    setObjectName("togglePill");
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setText(QString());
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(this, &QPushButton::toggled, this, [this](bool) { syncToggleState(); });
    syncToggleState();
}

void TogglePill::setOn(bool on) {
    setChecked(on);
}

bool TogglePill::isOn() const noexcept {
    return isChecked();
}

QSize TogglePill::sizeHint() const {
    return {58, 32};
}

QSize TogglePill::minimumSizeHint() const {
    return sizeHint();
}

void TogglePill::syncToggleState() {
    setProperty("toggleState", isChecked() ? "on" : "off");
    restyle(this);
}

} // namespace exosnap::ui::widgets
