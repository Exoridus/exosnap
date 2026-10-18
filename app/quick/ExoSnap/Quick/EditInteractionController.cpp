#include "EditInteractionController.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QStyleHints>
#include <cmath>

namespace exosnap::quick {

EditInteractionController::EditInteractionController(QObject* parent) : QObject(parent) {
}

EditInteractionController::~EditInteractionController() {
    if (scope_)
        disconnect(scope_, nullptr, this, nullptr);
    if (window_) {
        window_->removeEventFilter(this);
        disconnect(window_, nullptr, this, nullptr);
    }
}

void EditInteractionController::setScopeItem(QQuickItem* item) {
    if (scope_ == item)
        return;
    cancel();
    if (scope_)
        disconnect(scope_, nullptr, this, nullptr);
    scope_ = item;
    if (scope_) {
        connect(scope_, &QQuickItem::windowChanged, this, &EditInteractionController::bindWindow);
        connect(scope_, &QQuickItem::visibleChanged, this, [this] {
            if (!eligible())
                cancel();
        });
        connect(scope_, &QQuickItem::enabledChanged, this, [this] {
            if (!eligible())
                cancel();
        });
        connect(scope_, &QObject::destroyed, this, &EditInteractionController::bindWindow);
    }
    bindWindow();
    emit scopeItemChanged();
}

void EditInteractionController::bindWindow() {
    cancel();
    if (window_) {
        window_->removeEventFilter(this);
        disconnect(window_, nullptr, this, nullptr);
    }
    window_ = scope_ ? scope_->window() : nullptr;
    if (window_) {
        window_->installEventFilter(this);
        connect(window_, &QQuickWindow::activeFocusItemChanged, this, &EditInteractionController::cancel);
    }
}

bool EditInteractionController::eligible() const {
    if (blocked_ || !scope_ || !window_ || !scope_->isVisible() || !scope_->isEnabled())
        return false;
    const auto* focus = window_->activeFocusItem();
    return focus && (focus == scope_ || scope_->isAncestorOf(focus));
}

void EditInteractionController::setBlocked(bool value) {
    if (blocked_ == value)
        return;
    blocked_ = value;
    if (value)
        cancel();
    emit blockedChanged();
}

bool EditInteractionController::armSpace(int modifiers, bool auto_repeat) {
    if (auto_repeat)
        return held_;
    if (modifiers != Qt::NoModifier || !eligible() || QGuiApplication::mouseButtons() != Qt::NoButton)
        return false;
    if (!held_) {
        held_ = true;
        consumed_ = false;
        emit gestureChanged();
    }
    return true;
}

void EditInteractionController::beginPan(QPointF position) {
    if (!held_ || !eligible() || !std::isfinite(position.x()) || !std::isfinite(position.y()))
        return;
    consumed_ = true;
    captured_ = true;
    panning_ = false;
    press_ = previous_ = position;
    emit gestureChanged();
}

void EditInteractionController::updatePan(QPointF position) {
    if (!held_ || !captured_ || !std::isfinite(position.x()) || !std::isfinite(position.y()))
        return;
    if (!panning_ && (position - press_).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
        return;
    panning_ = true;
    const auto delta = previous_.x() - position.x();
    // Advance even at a viewport bound so reversing direction responds immediately.
    previous_ = position;
    emit panRequested(delta);
}

void EditInteractionController::endPan() {
    captured_ = false;
    panning_ = false;
    emit gestureChanged();
}

void EditInteractionController::cancel() {
    if (!held_)
        return;
    held_ = false;
    consumed_ = true;
    // Keep the pointer grab until release so cancellation cannot leak a clip click.
    emit gestureChanged();
}

bool EditInteractionController::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_ || !held_)
        return false;
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
        consumed_ = true;
    else if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        cancel();
    else if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() != Qt::Key_Space) {
            cancel();
            return key->key() == Qt::Key_Escape;
        }
    } else if (event->type() == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && !key->isAutoRepeat()) {
            const bool toggle = !consumed_ && eligible();
            held_ = false;
            emit gestureChanged();
            if (toggle)
                emit togglePlayRequested();
            return true;
        }
    }
    return false;
}

} // namespace exosnap::quick
