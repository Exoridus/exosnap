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
    UpdateAvailable,   // a newer release exists on the active channel (ADR 0012)
};

// ---------------------------------------------------------------------------
// NotificationAction
// ---------------------------------------------------------------------------
// A simple tagged action attached to a notification. Only two action IDs are
// needed at this scope. No heavy command pattern — callers dispatch based on
// the tag and call the appropriate service directly.
enum class NotificationAction : uint8_t {
    None,             // no action button
    OpenFolder,       // open the output folder in Explorer (Saved type)
    OpenRecovery,     // route to the existing recovery flow/overlay (RecoveryAvailable type)
    ChangeFolder,     // change output folder (LowStorage type)
    ShowFile,         // show / reveal the partial file (UnexpectedStop type)
    Discard,          // discard recovery session (secondary button on RecoveryAvailable)
    OpenUpdate,       // navigate to Settings → Software updates card (UpdateAvailable type)
    Edit,             // navigate to the Edit/Output page for the saved recording (primary on Saved type)
    RelaunchElevated, // relaunch ExoSnap as administrator to unlock elevation-gated diagnostics (ADR 0033)
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

    // Optional secondary action (e.g. "Discard" on RecoveryAvailable,
    // "Dismiss" on LowStorage). The primary action is in `action`.
    NotificationAction secondary_action = NotificationAction::None;

    // Stable ordering key assigned by the manager on enqueue — not set by callers.
    uint64_t sequence = 0;

    // Returns true if this event carries at least one actionable button.
    // Used by the tray unread badge to decide whether to increment the count.
    [[nodiscard]] bool hasAction() const noexcept {
        return action != NotificationAction::None || secondary_action != NotificationAction::None;
    }
};

} // namespace exosnap::notifications
