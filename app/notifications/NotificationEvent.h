#pragma once

#include <QString>
#include <cstdint>

namespace exosnap::notifications {

// ---------------------------------------------------------------------------
// NotificationType
// ---------------------------------------------------------------------------
// The four trigger sources for transient notification toasts (NOTIFY-TOASTS-R1).
// Each maps to exactly one wiring point in MainWindow.
enum class NotificationType : uint8_t {
    LowStorage,        // disk monitor crossed hard-stop threshold during recording
    Saved,             // recording finalized / saved successfully
    UnexpectedStop,    // recording stopped due to engine error (non-disk failure)
    RecoveryAvailable, // startup scan found recoverable sessions
};

// ---------------------------------------------------------------------------
// NotificationAction
// ---------------------------------------------------------------------------
// A simple tagged action attached to a notification. Only two action IDs are
// needed at this scope. No heavy command pattern — callers dispatch based on
// the tag and call the appropriate service directly.
enum class NotificationAction : uint8_t {
    None,         // no action button
    OpenFolder,   // open the output folder in Explorer (Saved type)
    OpenRecovery, // route to the existing recovery flow/overlay (RecoveryAvailable type)
};

// ---------------------------------------------------------------------------
// NotificationEvent
// ---------------------------------------------------------------------------
// Immutable data carried by a single toast. Produced at wiring sites and
// handed to NotificationManager::Enqueue().
struct NotificationEvent {
    NotificationType type = NotificationType::Saved;
    QString title;
    QString body;
    NotificationAction action = NotificationAction::None;
    // Extra payload for the action handler (e.g. file path for OpenFolder).
    QString action_payload;

    // Stable ordering key assigned by the manager on enqueue — not set by callers.
    uint64_t sequence = 0;
};

} // namespace exosnap::notifications
