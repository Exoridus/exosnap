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
    LowStorage,          // disk monitor crossed hard-stop threshold during recording
    Saved,               // recording finalized / saved successfully
    UnexpectedStop,      // recording stopped due to engine error (non-disk failure)
    RecoveryAvailable,   // startup scan found recoverable sessions
    UpdateAvailable,     // a newer release exists on the active channel (ADR 0012)
    FramesDropped,       // real frames lost during recording: encoder backpressure or a
                         // frame-processing failure (DROP-NOTIFY) — never benign CFR pacing
    SettingsRepaired,    // the preset store needed a field-wise repair on load
    PresetSwitched,      // a preset switch applied immediately; offers an Undo
    OverlayOmitted,      // this display's HDR10 format cannot carry the webcam/cursor overlays
    HotkeyConflict,      // a persisted global hotkey could not be registered at startup (held elsewhere)
    SettingsSaveFailed,  // a settings/preset write failed (disk full, file locked, ...) — the change may be lost
    AudioSourceDegraded, // an audio capture source lost its device mid-recording and is contributing
                         // honest silence while the engine retries (ADR 0046); standing while any
                         // source stays degraded, replaced in place if the degraded set changes,
                         // cleared the moment every source reactivates (or the recording ends).
    CaptureActionFailed, // a Record-page quick action (frame capture, split request) was rejected
                         // or failed; success is silent (the resulting file/segment is its own
                         // confirmation), only failures surface here.
    RecoveryProtectionUnavailable, // a recovery-manifest write did not reach disk, so this recording has
                                   // no crash-recovery entry. The recording itself is unaffected — same
                                   // class as a failed settings write: reported, never silent.
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
    OpenDiagnostics,  // navigate to the Diagnostics page for the frame-drop breakdown (FramesDropped type)
    UndoPresetSwitch, // restore the previous live config and selection (PresetSwitched type)
    OpenHotkeys,      // navigate to Settings → Hotkeys to rebind a shortcut (HotkeyConflict type)
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

    // Second action, an ordinary part of the model: with one action the toast
    // card itself is the action; with two, both become named buttons (e.g.
    // Edit + Open folder on Saved, Recover + Discard on RecoveryAvailable).
    NotificationAction secondary_action = NotificationAction::None;

    // Stable ordering key assigned by the manager on enqueue — not set by callers.
    uint64_t sequence = 0;

    // Returns true if this event carries at least one actionable button.
    // Used by the tray unread badge to decide whether to increment the count.
    [[nodiscard]] bool hasAction() const noexcept {
        return action != NotificationAction::None || secondary_action != NotificationAction::None;
    }
};

// ---------------------------------------------------------------------------
// AdvisoryStatusForType
// ---------------------------------------------------------------------------
// Pure resolver: notification type -> the hub's advisory status key
// ("success" / "caution" / "error" / "info").
//
// The status drives two things: the icon on the hub entry, and — since the
// bell went from a counted badge to a severity-coloured dot — the colour of the
// title-bar dot itself. That second consumer is why every type is listed
// explicitly instead of letting the harmless ones fall through a `default`:
// a failed settings write reported as "info" would leave the dot mint, which
// reads as "nothing is wrong". The switch has no default, so adding a
// NotificationType is a compile error until its severity is decided.
[[nodiscard]] inline QString AdvisoryStatusForType(NotificationType type) {
    switch (type) {
    case NotificationType::Saved:
        return QStringLiteral("success");

    // Something failed, was lost, or ended the recording.
    case NotificationType::LowStorage: // crosses the hard-stop threshold and stops recording
    case NotificationType::UnexpectedStop:
    case NotificationType::SettingsSaveFailed:  // the change may be lost
    case NotificationType::CaptureActionFailed: // the requested action did not happen
        return QStringLiteral("error");

    // Degraded, but the user keeps working — including RecoveryAvailable, which
    // offers to rescue work rather than reporting a live failure. As "error" it
    // would paint the bell coral within seconds of every launch that finds a
    // recoverable session.
    case NotificationType::FramesDropped:
    case NotificationType::OverlayOmitted:
    case NotificationType::AudioSourceDegraded:
    case NotificationType::RecoveryAvailable:
    case NotificationType::HotkeyConflict:   // a bound hotkey is dead
    case NotificationType::SettingsRepaired: // the store needed repairing on load
    // The recording is fine; only the crash-recovery safety net is missing. Coral
    // would claim the recording failed, which it did not.
    case NotificationType::RecoveryProtectionUnavailable:
        return QStringLiteral("caution");

    case NotificationType::UpdateAvailable:
    case NotificationType::PresetSwitched:
        return QStringLiteral("info");
    }
    // Only reachable through a cast from an out-of-range value.
    return QStringLiteral("info");
}

// ---------------------------------------------------------------------------
// MakeAudioSourceDegradedEvent
// ---------------------------------------------------------------------------
// Pure resolver: degraded-source count -> the standing AudioSourceDegraded toast
// body (ADR 0046). Kept separate from the MainWindow wiring that decides WHEN to
// call it, so the text itself is unit-testable without constructing a window
// (CLAUDE.md: prefer explicit models and pure resolver logic where possible).
// degraded_count must be >= 1 — the caller (MainWindow) never raises this
// notification for a count of 0; it dismisses the standing toast instead.
[[nodiscard]] inline NotificationEvent MakeAudioSourceDegradedEvent(uint32_t degraded_count) {
    NotificationEvent event;
    event.type = NotificationType::AudioSourceDegraded;
    event.title =
        degraded_count == 1 ? QStringLiteral("Audio source went silent") : QStringLiteral("Audio sources went silent");
    event.body = degraded_count == 1 ? QStringLiteral("An audio source lost its device. Recording continues — "
                                                      "ExoSnap keeps retrying the connection.")
                                     : QStringLiteral("%1 audio sources lost their device. Recording continues — "
                                                      "ExoSnap keeps retrying the connections.")
                                           .arg(degraded_count);
    event.action = NotificationAction::OpenDiagnostics;
    return event;
}

} // namespace exosnap::notifications
