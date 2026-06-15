#include "NotificationManager.h"

#include <QDateTime>

namespace exosnap::notifications {

NotificationManager::NotificationManager(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &NotificationManager::onTimerFired);
}

void NotificationManager::Enqueue(NotificationEvent event) {
    event.sequence = next_sequence_++;
    pending_queue_.push_back(std::move(event));
    drainQueue();
}

void NotificationManager::Dismiss(uint64_t sequence) {
    for (int i = 0; i < visible_.size(); ++i) {
        if (visible_[i].sequence == sequence) {
            visible_.remove(i);
            visible_shown_at_.remove(i);
            drainQueue();
            rescheduleTimer();
            emit visibleSetChanged();
            return;
        }
    }
}

const QVector<NotificationEvent>& NotificationManager::VisibleEvents() const noexcept {
    return visible_;
}

int NotificationManager::PendingCount() const noexcept {
    return static_cast<int>(pending_queue_.size());
}

// static
int NotificationManager::DismissIntervalMs(NotificationType type) noexcept {
    switch (type) {
    case NotificationType::Saved:
        return kDismissMs_Saved;
    case NotificationType::LowStorage:
        return kDismissMs_LowStorage;
    case NotificationType::UnexpectedStop:
        return kDismissMs_UnexpectedStop; // sticky
    case NotificationType::RecoveryAvailable:
        return kDismissMs_RecoveryAvailable; // sticky
    case NotificationType::UpdateAvailable:
        return kDismissMs_UpdateAvailable; // timed (8 s)
    }
    return kDismissMs_Saved;
}

void NotificationManager::drainQueue() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;

    while (!pending_queue_.empty() && visible_.size() < kMaxVisible) {
        NotificationEvent event = std::move(pending_queue_.front()); // NOLINT(performance-move-const-arg)
        pending_queue_.pop_front();
        const bool has_action = event.hasAction();
        visible_.push_back(std::move(event));
        visible_shown_at_.push_back(now_ms);
        changed = true;
        // Notify the tray badge when an actionable toast becomes visible.
        if (has_action)
            emit actionableEventShown();
    }

    if (changed) {
        rescheduleTimer();
        emit visibleSetChanged();
    }
}

void NotificationManager::rescheduleTimer() {
    timer_->stop();

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    qint64 earliest_expiry = -1; // -1 = no auto-dismiss scheduled

    for (int i = 0; i < visible_.size(); ++i) {
        const int duration = DismissIntervalMs(visible_[i].type);
        if (duration <= 0)
            continue; // sticky — never auto-dismiss

        const qint64 expiry = visible_shown_at_[i] + static_cast<qint64>(duration);
        if (earliest_expiry < 0 || expiry < earliest_expiry) {
            earliest_expiry = expiry;
        }
    }

    if (earliest_expiry >= 0) {
        const qint64 delay = qMax<qint64>(0, earliest_expiry - now_ms);
        timer_->start(static_cast<int>(delay));
    }
}

void NotificationManager::onTimerFired() {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;

    // Remove all visible events whose auto-dismiss time has passed.
    // Iterate backwards to avoid index shifting.
    for (int i = visible_.size() - 1; i >= 0; --i) {
        const int duration = DismissIntervalMs(visible_[i].type);
        if (duration <= 0)
            continue; // sticky

        const qint64 expiry = visible_shown_at_[i] + static_cast<qint64>(duration);
        if (now_ms >= expiry) {
            visible_.remove(i);
            visible_shown_at_.remove(i);
            changed = true;
        }
    }

    if (changed) {
        drainQueue(); // may promote queued events into newly-freed slots
        // drainQueue() emits visibleSetChanged() and reschedules the timer
        // if there are new visible items. If it promoted nothing, emit here.
        // Note: drainQueue emits only when it adds something. If it added
        // nothing but we removed something above, we still need to emit.
        // Safe to emit twice (idempotent for the window).
        emit visibleSetChanged();
        rescheduleTimer();
    }
}

} // namespace exosnap::notifications
