#include "NotificationManager.h"

#include <QDateTime>

namespace exosnap::notifications {

NotificationManager::NotificationManager(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    // Precise, not the default coarse timer. Qt gives a coarse timer 5 % of
    // slack in EITHER direction, which for a ten-second toast is half a second
    // early -- and an early wake-up finds nothing expired.
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &NotificationManager::onTimerFired);
}

uint64_t NotificationManager::Enqueue(NotificationEvent event) {
    event.sequence = next_sequence_++;
    const uint64_t sequence = event.sequence;

    // The hub is the record: every event is announced, always.
    emit eventRecorded(event);

    // PresetSwitched is record-only: the combo box that performed the switch
    // already offers the way back, so a toast would be noise.
    if (event.type == NotificationType::PresetSwitched)
        return sequence;

    if (!toasts_enabled_)
        return sequence;

    const bool has_action = event.hasAction();

    if (IsStanding(event.type)) {
        // Standing toasts stack above the timed slot: insert before a trailing
        // timed toast so the timed one stays the last (anchor-nearest) card.
        int insert_at = visible_.size();
        if (!visible_.isEmpty() && !IsStanding(visible_.last().type))
            insert_at -= 1;
        visible_.insert(insert_at, std::move(event));
        visible_shown_at_.insert(insert_at, QDateTime::currentMSecsSinceEpoch());
    } else {
        // At most one timed toast: a new one replaces the current one and
        // never displaces a standing toast.
        if (!visible_.isEmpty() && !IsStanding(visible_.last().type)) {
            visible_.removeLast();
            visible_shown_at_.removeLast();
        }
        visible_.push_back(std::move(event));
        visible_shown_at_.push_back(QDateTime::currentMSecsSinceEpoch());
    }

    if (has_action)
        emit actionableEventShown();

    rescheduleTimer();
    emit visibleSetChanged();
    return sequence;
}

void NotificationManager::Dismiss(uint64_t sequence) {
    for (int i = 0; i < visible_.size(); ++i) {
        if (visible_[i].sequence == sequence) {
            visible_.remove(i);
            visible_shown_at_.remove(i);
            rescheduleTimer();
            emit visibleSetChanged();
            return;
        }
    }
}

void NotificationManager::SetToastsEnabled(bool enabled) {
    toasts_enabled_ = enabled;
    if (!enabled && !visible_.isEmpty()) {
        visible_.clear();
        visible_shown_at_.clear();
        rescheduleTimer();
        emit visibleSetChanged();
    }
}

const QVector<NotificationEvent>& NotificationManager::VisibleEvents() const noexcept {
    return visible_;
}

qint64 NotificationManager::ShownAtMs(uint64_t sequence) const noexcept {
    for (int i = 0; i < visible_.size(); ++i) {
        if (visible_[i].sequence == sequence)
            return visible_shown_at_[i];
    }
    return -1;
}

// static
int NotificationManager::DismissIntervalMs(NotificationType type) noexcept {
    switch (type) {
    case NotificationType::Saved:
        return kDismissMs_Saved;
    case NotificationType::LowStorage:
        return kDismissMs_LowStorage;
    case NotificationType::UnexpectedStop:
        return kDismissMs_UnexpectedStop;
    case NotificationType::RecoveryAvailable:
        return kDismissMs_RecoveryAvailable;
    case NotificationType::UpdateAvailable:
        return kDismissMs_UpdateAvailable;
    case NotificationType::FramesDropped:
        return kDismissMs_FramesDropped;
    case NotificationType::SettingsRepaired:
        return kDismissMs_SettingsRepaired;
    case NotificationType::PresetSwitched:
        return kDismissMs_PresetSwitched;
    case NotificationType::OverlayOmitted:
        return kDismissMs_OverlayOmitted;
    case NotificationType::HotkeyConflict:
        return kDismissMs_HotkeyConflict;
    case NotificationType::SettingsSaveFailed:
        return kDismissMs_SettingsSaveFailed;
    case NotificationType::AudioSourceDegraded:
        return kDismissMs_AudioSourceDegraded;
    case NotificationType::AudioDefaultDeviceChanged:
        return kDismissMs_AudioDefaultDeviceChanged;
    case NotificationType::FrameCaptured:
        return kDismissMs_FrameCaptured;
    case NotificationType::CaptureActionFailed:
        return kDismissMs_CaptureActionFailed;
    case NotificationType::PresetTransferFailed:
        return kDismissMs_PresetTransferFailed;
    case NotificationType::RecoveryProtectionUnavailable:
        return kDismissMs_RecoveryProtectionUnavailable;
    case NotificationType::SettingsLoadFailed:
        return kDismissMs_SettingsLoadFailed;
    case NotificationType::WindowCaptureStalled:
        return kDismissMs_WindowCaptureStalled;
    }
    return kDismissMs_Saved;
}

// static
bool NotificationManager::IsStanding(NotificationType type) noexcept {
    return DismissIntervalMs(type) == 0;
}

void NotificationManager::FireDismissTimerForTest() {
    // A single-shot timer is already stopped by the time its handler runs.
    // Stopping it here is what makes this an early WAKE-UP rather than a bare
    // call into the handler -- without it the test cannot see the defect,
    // because the timer it should have re-armed was never disarmed.
    timer_->stop();
    onTimerFired();
}

bool NotificationManager::DismissTimerArmedForTest() const {
    return timer_ != nullptr && timer_->isActive();
}

void NotificationManager::rescheduleTimer() {
    timer_->stop();

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    qint64 earliest_expiry = -1; // -1 = no auto-dismiss scheduled

    for (int i = 0; i < visible_.size(); ++i) {
        const int duration = DismissIntervalMs(visible_[i].type);
        if (duration <= 0)
            continue; // standing — never auto-dismiss

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
            continue; // standing

        const qint64 expiry = visible_shown_at_[i] + static_cast<qint64>(duration);
        if (now_ms >= expiry) {
            visible_.remove(i);
            visible_shown_at_.remove(i);
            changed = true;
        }
    }

    // Rearmed unconditionally, and that is the fix rather than a tidy-up. A
    // timer that fires a millisecond early finds nothing expired; rescheduling
    // only on a change left the toast on screen with nothing armed to remove it,
    // which is exactly how a timed toast became a permanent one.
    rescheduleTimer();
    if (changed)
        emit visibleSetChanged();
}

} // namespace exosnap::notifications
