#include "ComboBoxWheelFilter.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QWheelEvent>
#include <QWidget>

namespace exosnap::ui::widgets {

ComboBoxWheelFilter::ComboBoxWheelFilter(QObject* parent) : QObject(parent) {
}

void ComboBoxWheelFilter::installOn(QComboBox* combo) {
    if (!combo) {
        return;
    }
    combo->setFocusPolicy(Qt::StrongFocus);
    combo->installEventFilter(this);
}

bool ComboBoxWheelFilter::eventFilter(QObject* watched, QEvent* event) {
    auto* combo = qobject_cast<QComboBox*>(watched);
    if (combo == nullptr || event->type() != QEvent::Wheel) {
        return QObject::eventFilter(watched, event);
    }

    const bool popup_open = combo->view() != nullptr && combo->view()->isVisible();
    if (popup_open || combo->hasFocus()) {
        return QObject::eventFilter(watched, event);
    }

    auto* wheel_event = static_cast<QWheelEvent*>(event);
    event->ignore();
    forwardWheelToScrollArea(combo, wheel_event);
    return true;
}

void ComboBoxWheelFilter::forwardWheelToScrollArea(QComboBox* combo, QWheelEvent* wheel_event) const {
    if (combo == nullptr || wheel_event == nullptr) {
        return;
    }

    QWidget* parent = combo->parentWidget();
    while (parent != nullptr) {
        auto* scroll_area = qobject_cast<QAbstractScrollArea*>(parent);
        if (scroll_area != nullptr && scroll_area->viewport() != nullptr) {
            const QPointF combo_local = wheel_event->position();
            const QPoint global_pos = combo->mapToGlobal(combo_local.toPoint());
            const QPoint viewport_pos = scroll_area->viewport()->mapFromGlobal(global_pos);
            QWheelEvent forwarded_event(QPointF(viewport_pos), QPointF(global_pos), wheel_event->pixelDelta(),
                                        wheel_event->angleDelta(), wheel_event->buttons(), wheel_event->modifiers(),
                                        wheel_event->phase(), wheel_event->inverted(), wheel_event->source(),
                                        wheel_event->pointingDevice());
            QApplication::sendEvent(scroll_area->viewport(), &forwarded_event);
            return;
        }
        parent = parent->parentWidget();
    }
}

} // namespace exosnap::ui::widgets
