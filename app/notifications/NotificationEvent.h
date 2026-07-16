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
    FramesDropped,       // encoder backpressure dropped real frames during recording (DROP-NOTIFY)
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
