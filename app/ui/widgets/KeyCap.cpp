#include "KeyCap.h"

namespace exosnap::ui::widgets {

KeyCap::KeyCap(const QString& key, QWidget* parent) : QLabel(key, parent), key_(key) {
    setProperty("widgetRole", "keyCap");
    setContentsMargins(9, 4, 9, 4);
    setAlignment(Qt::AlignCenter);
}

KeyCap::KeyCap(QWidget* parent) : QLabel(parent) {
    setProperty("widgetRole", "keyCap");
    setContentsMargins(9, 4, 9, 4);
    setAlignment(Qt::AlignCenter);
}

void KeyCap::setKey(const QString& key) {
    key_ = key;
    setText(key);
}

const QString& KeyCap::key() const {
    return key_;
}

} // namespace exosnap::ui::widgets
