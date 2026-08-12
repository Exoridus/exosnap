#include "NotificationsAdapter.h"

namespace exosnap::quick {

NotificationsAdapter::NotificationsAdapter(QObject* parent)
    : QObject(parent), manager_(std::make_unique<notifications::NotificationManager>()) {
    connect(manager_.get(), &notifications::NotificationManager::eventRecorded, this,
            &NotificationsAdapter::onEventRecorded);

    const auto entries_changed = [this]() { emit entriesChanged(); };
    connect(&model_, &QAbstractItemModel::rowsInserted, this, entries_changed);
    connect(&model_, &QAbstractItemModel::rowsRemoved, this, entries_changed);
    connect(&model_, &QAbstractItemModel::modelReset, this, entries_changed);

    toast_model_.attach(manager_.get());
}

QAbstractItemModel* NotificationsAdapter::model() noexcept {
    return &model_;
}

QAbstractItemModel* NotificationsAdapter::toastModel() noexcept {
    return &toast_model_;
}

const QRect& NotificationsAdapter::toastAnchorGeometry() const noexcept {
    return toast_anchor_geometry_;
}

void NotificationsAdapter::setToastAnchorGeometry(const QRect& geometry) {
    if (toast_anchor_geometry_ == geometry)
        return;
    toast_anchor_geometry_ = geometry;
    emit toastAnchorChanged();
}

int NotificationsAdapter::unreadCount() const noexcept {
    return model_.unreadCount();
}

QString NotificationsAdapter::worstUnreadTone() const {
    return model_.worstUnreadTone();
}

bool NotificationsAdapter::hubOpen() const noexcept {
    return hub_open_;
}

bool NotificationsAdapter::hasEntries() const noexcept {
    return model_.rowCount() > 0;
}

void NotificationsAdapter::openHub() {
    if (hub_open_)
        return;
    hub_open_ = true;
    emit hubOpenChanged();
}

void NotificationsAdapter::closeHub() {
    if (!hub_open_)
        return;
    hub_open_ = false;
    emit hubOpenChanged();
}

void NotificationsAdapter::toggleHub() {
    if (hub_open_)
        closeHub();
    else
        openHub();
}

void NotificationsAdapter::markAllRead() {
    model_.markAllRead();
    emit unreadChanged();
}

void NotificationsAdapter::dismissEntry(int index) {
    model_.removeAt(index);
    // The removed row may have been unread — cheap to recompute unconditionally
    // rather than tracking whether it was (same idiom LogsAdapter uses for its
    // counts_changed handler on every row-structure change).
    emit unreadChanged();
}

void NotificationsAdapter::dismissAll() {
    model_.clear();
    emit unreadChanged();
}

void NotificationsAdapter::triggerAction(int index, int action) {
    const notifications::NotificationEvent& event = model_.eventAt(index);
    const auto requested = static_cast<notifications::NotificationAction>(action);
    if (requested == notifications::NotificationAction::None)
        return;
    if (requested != event.action && requested != event.secondary_action)
        return;

    model_.markRead(index);
    emit unreadChanged();
    emit actionTriggered(requested, event.action_payload);
}

void NotificationsAdapter::triggerToastAction(qint64 sequence, int action) {
    const notifications::NotificationEvent* event = toast_model_.eventForSequence(static_cast<quint64>(sequence));
    if (event == nullptr)
        return;
    const auto requested = static_cast<notifications::NotificationAction>(action);
    if (requested == notifications::NotificationAction::None)
        return;
    if (requested != event->action && requested != event->secondary_action)
        return;

    const QString payload = event->action_payload;
    // Dismissed first: acting on a toast retires it, and the manager's own
    // bookkeeping is what the hub badge and this model both read. Note the
    // payload copy above — Dismiss() invalidates the pointer.
    manager_->Dismiss(static_cast<quint64>(sequence));
    emit actionTriggered(requested, payload);
}

void NotificationsAdapter::dismissToast(qint64 sequence) {
    manager_->Dismiss(static_cast<quint64>(sequence));
}

notifications::NotificationManager& NotificationsAdapter::manager() noexcept {
    return *manager_;
}

bool NotificationsAdapter::removeEntryByKey(const QString& key) {
    const bool removed = model_.removeByKey(key);
    if (removed)
        emit unreadChanged();
    return removed;
}

void NotificationsAdapter::onEventRecorded(const notifications::NotificationEvent& event) {
    model_.recordEvent(event);
    emit unreadChanged();
}

} // namespace exosnap::quick
