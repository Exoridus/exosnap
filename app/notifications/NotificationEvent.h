#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

#include <exosnap/engine/audio_track_model.h>

namespace exosnap::notifications {

// ---------------------------------------------------------------------------
// NotificationType
// ---------------------------------------------------------------------------
// The four trigger sources for transient notification toasts (NOTIFY-TOASTS-R1).
// Each maps to exactly one wiring point in MainWindow.
enum class NotificationType : uint8_t {
    LowStorage,                // disk monitor crossed hard-stop threshold during recording
    Saved,                     // recording finalized / saved successfully
    UnexpectedStop,            // recording stopped due to engine error (non-disk failure)
    RecoveryAvailable,         // startup scan found recoverable sessions
    UpdateAvailable,           // a newer release exists on the active channel (ADR 0012)
    FramesDropped,             // real frames lost during recording: encoder backpressure or a
                               // frame-processing failure (DROP-NOTIFY) — never benign CFR pacing
    SettingsRepaired,          // the preset store needed a field-wise repair on load
    PresetSwitched,            // a preset switch applied immediately; offers an Undo
    OverlayOmitted,            // this display's HDR10 format cannot carry the webcam/cursor overlays
    HotkeyConflict,            // a persisted global hotkey could not be registered at startup (held elsewhere)
    SettingsSaveFailed,        // a settings/preset write failed (disk full, file locked, ...) — the change may be lost
    AudioDefaultDeviceChanged, // Windows switched the default microphone mid-recording; the session keeps its device
    AudioSourceDegraded,       // an audio capture source lost its device mid-recording and is contributing
                               // honest silence while the engine retries (ADR 0046); standing while any
                               // source stays degraded, replaced in place if the degraded set changes,
                               // cleared the moment every source reactivates (or the recording ends).
    CaptureActionFailed,       // a Record-page quick action (frame capture, split request) was rejected
    FrameCaptured,             // a single frame was written to disk from the Record page
                               // or failed; success is silent (the resulting file/segment is its own
                               // confirmation), only failures surface here.
    RecoveryProtectionUnavailable, // a recovery-manifest write did not reach disk, so this recording has
                                   // no crash-recovery entry. The recording itself is unaffected — same
                                   // class as a failed settings write: reported, never silent.
    SettingsLoadFailed,            // settings.ini exists but could not be read, so this session runs on
                                   // built-in defaults. Distinct from SettingsRepaired (which recovered
                                   // what it could) and from SettingsSaveFailed (a write that was lost):
                                   // nothing has been written yet, and nothing will be until the user
                                   // deliberately changes a setting.
    PresetTransferFailed,          // importing or exporting a preset file failed. Nothing in the live
                                   // configuration changed, so this reports an action that did not happen
                                   // rather than a setting that was lost.
    WindowCaptureStalled,          // an active WINDOW capture stopped producing frames mid-recording
                                   // (QCR-804). The recording keeps running and the file keeps growing —
                                   // the CFR pacer holds the last frame — so this is a standing caution,
                                   // never a failure. Cleared the moment frames resume, and when the
                                   // recording ends.
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
    SendReport,       // send a scrubbed non-fatal report for an internal failure the user cannot fix.
                      // Pressing it IS the consent, the same rule the recording-error surface follows;
                      // `action_payload` carries the detail line the report is built from.
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
    case NotificationType::FrameCaptured:
        return QStringLiteral("success");

    // Something failed, was lost, or ended the recording.
    case NotificationType::LowStorage: // crosses the hard-stop threshold and stops recording
    case NotificationType::UnexpectedStop:
    case NotificationType::SettingsSaveFailed:   // the change may be lost
    case NotificationType::CaptureActionFailed:  // the requested action did not happen
    case NotificationType::PresetTransferFailed: // the import or export did not happen
        return QStringLiteral("error");

    // Degraded, but the user keeps working — including RecoveryAvailable, which
    // offers to rescue work rather than reporting a live failure. As "error" it
    // would paint the bell coral within seconds of every launch that finds a
    // recoverable session.
    case NotificationType::FramesDropped:
    case NotificationType::OverlayOmitted:
    case NotificationType::AudioSourceDegraded:
    case NotificationType::AudioDefaultDeviceChanged:
    case NotificationType::RecoveryAvailable:
    case NotificationType::HotkeyConflict:   // a bound hotkey is dead
    case NotificationType::SettingsRepaired: // the store needed repairing on load
    // The recording is fine; only the crash-recovery safety net is missing. Coral
    // would claim the recording failed, which it did not.
    case NotificationType::RecoveryProtectionUnavailable:
    // Nothing was lost: the unreadable file is still on disk and is deliberately
    // not being written over. Coral would claim a destroyed configuration.
    case NotificationType::SettingsLoadFailed:
    // The recording is still running and still being written. Coral would claim
    // it failed, which it did not — and the stall may resolve by itself.
    case NotificationType::WindowCaptureStalled:
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
[[nodiscard]] inline NotificationEvent MakeAudioSourceDegradedEvent(uint32_t degraded_count,
                                                                    uint32_t degraded_source_kinds = 0) {
    NotificationEvent event;
    event.type = NotificationType::AudioSourceDegraded;
    QStringList sources;
    using exosnap::engine::AudioSourceKind;
    using exosnap::engine::AudioSourceKindBit;
    if ((degraded_source_kinds & AudioSourceKindBit(AudioSourceKind::Mic)) != 0)
        sources << QStringLiteral("Microphone");
    if ((degraded_source_kinds &
         (AudioSourceKindBit(AudioSourceKind::Sys) | AudioSourceKindBit(AudioSourceKind::SystemOutput))) != 0)
        sources << QStringLiteral("System audio");
    if ((degraded_source_kinds & AudioSourceKindBit(AudioSourceKind::App)) != 0)
        sources << QStringLiteral("Application audio");
    if (!sources.isEmpty()) {
        event.title = sources.size() == 1 ? QStringLiteral("%1 went silent").arg(sources.front())
                                          : QStringLiteral("Audio sources went silent");
        event.body = QStringLiteral("%1 lost %2 device. Recording continues without %3 while ExoSnap retries the "
                                    "connection.")
                         .arg(sources.join(QStringLiteral(", ")),
                              sources.size() == 1 ? QStringLiteral("its") : QStringLiteral("their"),
                              sources.size() == 1 ? QStringLiteral("it") : QStringLiteral("them"));
        event.action = NotificationAction::OpenDiagnostics;
        return event;
    }
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

// ---------------------------------------------------------------------------
// MakeWindowCaptureStalledEvent
// ---------------------------------------------------------------------------
// Pure resolver: the standing mid-recording capture-stall notice (QCR-804).
// Same division of labour as MakeAudioSourceDegradedEvent — the composition root
// decides WHEN, this decides WHAT IS SAID — so the wording is unit-testable
// without a recording.
//
// Truthfulness rules baked into the text, in order of importance:
//   * "appears to have stalled" / "may be frozen": what ExoSnap measured is the
//     absence of frame progress. It has NOT proven the picture is frozen, and it
//     has NOT proven a cause.
//   * The recording is explicitly stated to be still running. This is not a
//     failure report; the file keeps growing and Stop/Pause still work.
//   * The exclusive-fullscreen sentence appears ONLY when a fullscreen signal
//     (QUNS or PresentMon) actually corroborated it, and even then it is phrased
//     as a conditional the user can check — never "exclusive fullscreen detected".
//
// seconds_without_frames is the measured starve duration, rounded for display.
[[nodiscard]] inline NotificationEvent MakeWindowCaptureStalledEvent(double seconds_without_frames,
                                                                     bool exclusive_fullscreen_hint) {
    NotificationEvent event;
    event.type = NotificationType::WindowCaptureStalled;
    event.title = QStringLiteral("Window capture appears to have stalled");
    const int seconds = static_cast<int>(seconds_without_frames + 0.5);
    event.body = QStringLiteral("No new frame has arrived from the captured window for %1 seconds. The recording is "
                                "still running, but the captured window may be frozen.")
                     .arg(seconds);
    if (exclusive_fullscreen_hint) {
        event.body += QStringLiteral(" If the application switched to exclusive fullscreen, set it back to windowed "
                                     "or borderless mode — or stop the recording.");
    }
    event.action = NotificationAction::OpenDiagnostics;
    return event;
}

// Windows moved the default microphone while a session that followed the
// default was recording. The session does not follow: a mid-file device switch
// would change format and clock under a running encoder. Said plainly, so the
// user does not conclude the recorder ignored the switch.
[[nodiscard]] inline NotificationEvent MakeAudioDefaultDeviceChangedEvent(const QString& new_device,
                                                                          const QString& kept_device) {
    NotificationEvent event;
    event.type = NotificationType::AudioDefaultDeviceChanged;
    event.title = QStringLiteral("Default microphone changed");
    const QString to =
        new_device.isEmpty() ? QStringLiteral("another device") : QStringLiteral("\"%1\"").arg(new_device);
    const QString kept = kept_device.isEmpty() ? QStringLiteral("the device it started with")
                                               : QStringLiteral("\"%1\"").arg(kept_device);
    event.body = QStringLiteral("Windows switched the default microphone to %1 while recording. This recording keeps "
                                "capturing %2; it will not switch mid-file.")
                     .arg(to, kept);
    event.action = NotificationAction::OpenDiagnostics;
    return event;
}

// The display-capture counterpart. Raised only with corroboration (the console
// display is off), so the text can say what was measured: no frame, and a
// display that is not producing one. Wording rules as above: the recording is
// running, the file grows, the source may recover on its own.
[[nodiscard]] inline NotificationEvent MakeDisplayCaptureStalledEvent(double seconds_without_frames, bool display_off) {
    NotificationEvent event;
    event.type = NotificationType::WindowCaptureStalled;
    event.title = QStringLiteral("Display capture appears to have stalled");
    const int seconds = static_cast<int>(seconds_without_frames + 0.5);
    event.body = QStringLiteral("No new frame has arrived from the captured display for %1 seconds. The recording "
                                "is still running and holds the last picture.")
                     .arg(seconds);
    if (display_off) {
        event.body += QStringLiteral(" The display is off or asleep; wake it, or stop the recording.");
    }
    event.action = NotificationAction::OpenDiagnostics;
    return event;
}

} // namespace exosnap::notifications
