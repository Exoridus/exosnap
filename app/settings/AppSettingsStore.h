#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <string>

#include "../models/CrashReportPolicy.h"

namespace exosnap {

struct PersistedWindowGeometry {
    int x = -1;
    int y = -1;
    int width = -1;
    int height = -1;
    bool maximized = false;
};

// What Load() actually found. The three states are not interchangeable: only
// `ReadFailed` means "there is a settings file we could not read", and only that
// state must inhibit an unattended overwrite. A missing file is a legitimate
// first run whose defaults may be written back freely.
enum class SettingsLoadOutcome : uint8_t {
    // The settings file was read without error.
    Loaded,
    // No settings file exists yet (first run, or a wiped config dir). Every
    // field is the built-in default and persisting them is correct.
    DefaultsNoFile,
    // A settings file exists but QSettings reported an error while reading it
    // (corrupt, locked, unreadable). Every field is the built-in default rather
    // than a faithful read of whatever is on disk, so writing them back would
    // replace the user's configuration with defaults.
    ReadFailed,
};

// Why a write has to declare its intent (QCR-201): after a load that failed,
// the in-memory settings are the built-in defaults, and almost every write the
// app performs is one the user never asked for — the window-geometry debounce
// fires on every move and on close, a one-time tray flag persists itself, a
// hotkey that could not be registered is dropped and saved. Launching and
// quitting the app was therefore enough to replace an unreadable settings file
// with defaults.
enum class SettingsWriteIntent : uint8_t {
    // Housekeeping the app does for itself.
    Incidental,
    // The user edited a setting and expects it to stick.
    UserEdit,
};

enum class SettingsWriteDecision : uint8_t {
    // Persist normally.
    Write,
    // Persist, but move the unreadable file aside first so the user's own
    // configuration survives the decision to start a fresh one.
    PreserveThenWrite,
    // Do not touch the file.
    Refuse,
};

// The QCR-201 decision, as a pure function of what the load found, whether a
// deliberate edit has already superseded a failed load in this session, and
// what kind of write is being attempted.
[[nodiscard]] constexpr SettingsWriteDecision ResolveSettingsWrite(SettingsLoadOutcome outcome, bool superseded,
                                                                   SettingsWriteIntent intent) {
    // A missing file and a clean read are both faithful pictures of the store;
    // so is a failed load the user has already deliberately written over.
    if (outcome != SettingsLoadOutcome::ReadFailed || superseded)
        return SettingsWriteDecision::Write;
    // The user's own edit outranks a file nobody can read — but not silently.
    if (intent == SettingsWriteIntent::UserEdit)
        return SettingsWriteDecision::PreserveThenWrite;
    return SettingsWriteDecision::Refuse;
}

struct PersistedAppSettings {
    // Indexed by HotkeyAction: ToggleRecording, TogglePause, CaptureFrame,
    // AddMarker, SplitRecording. Size must match kHotkeyActionCount.
    std::array<QString, 5> hotkey_bindings = {
        QString(), QString(), QString(), QString(), QString(),
    };
    PersistedWindowGeometry window_geometry;

    // RECORDING-OVERLAY-R1: whether the on-screen recording status overlay is
    // shown during recording / paused. Default ON. Excluded from capture via
    // SetWindowDisplayAffinity; hidden on failure.
    bool show_recording_overlay = true;

    // DIAGNOSTICS-OVERLAY-R1: whether the live diagnostics overlay (fps, A/V
    // drift, dropped frames, output size, muted-source glyphs) is shown during
    // recording / paused. Default OFF. Excluded from capture via
    // SetWindowDisplayAffinity; hidden on failure.
    bool show_diagnostics_overlay = false;

    // What the two overlays above put on screen. Presets persist as their token
    // ("minimal" / "health" / "technical" / "custom"); the custom sets persist as
    // a comma-separated element-token list. The vocabulary and the resolution
    // both live in models/OverlayContentPolicy — this struct only carries them.
    //
    // A token list rather than one bool per element: the settings file stays
    // readable, and adding an element does not add a key. The custom lists are
    // kept even while a named preset is selected, so switching away and back
    // returns the user's own set.
    QString recording_overlay_preset = QStringLiteral("minimal");
    QString recording_overlay_custom_elements = QStringLiteral("elapsed");
    QString diagnostics_overlay_preset = QStringLiteral("health");
    QString diagnostics_overlay_custom_elements = QStringLiteral("drop,drift,muted");

    // NOTIFY-TOASTS-R1: whether transient notification toasts are shown.
    // Default ON. Excluded from capture via SetWindowDisplayAffinity.
    // Covers: LowStorage, Saved, UnexpectedStop, RecoveryAvailable.
    bool show_notifications = true;

    // "Open editor when finished" (Output card): when a recording completes
    // successfully, open the Edit overlay directly instead of showing the
    // "Recording saved" toast with Edit/Show-in-folder actions. Default ON
    // (the product's post-record path is editing, not the toast). Was
    // previously an ADR-0031 debug-only roadmap-dummy toggle with no engine
    // setting behind it at all.
    bool open_editor_when_finished = true;

    // Whether MINIMIZING hides the window to the tray instead of sending it to
    // the taskbar. Default OFF. Closing is not affected by any preference: the
    // close button always closes.
    bool minimize_to_tray = false;

    // Whether the shell window carries WDA_EXCLUDEFROMCAPTURE. Default OFF.
    // Unconditional while on -- not scoped to a recording -- so the label
    // ("Hide the ExoSnap window from screen capture") is true whenever the
    // setting is. Reaches every screen capture on the machine, ExoSnap's own and
    // any other application's. The five overlays are excluded regardless and
    // this setting does not touch them.
    bool hide_window_from_capture = false;

    // QUICK-PILL-R1: whether the interactive quick-control pill overlay is shown
    // during recording. Default OFF.  The pill is capture-excluded (via
    // SetWindowDisplayAffinity) and interactive (NOT click-through), so it is an
    // opt-in feature gated here.
    bool show_quick_controls = false;

    // CRASH-POLICY-R2 (ADR 0017): explicit persisted report policy. AskEveryTime
    // is the privacy-by-default state; NeverSend is an explicit refusal and
    // suppresses only the report-consent prompt, never local recovery UI.
    CrashReportPolicy crash_report_policy = CrashReportPolicy::AskEveryTime;

    // UPDATE-WIRE-R1 (ADR 0012): the selected update channel — "Stable" | "Preview".
    // Applied immediately on change: persisted, pushed into UpdateService, and
    // the previous channel's answer is dropped from the card. No automatic
    // re-check — a network check stays the user's explicit action (ADR 0045).
    // Default Stable.
    QString update_channel = QStringLiteral("Stable");

    // UPDATE-WIRE-R1 (ADR 0012): whether to run a guarded update check on startup.
    // Default OFF (ADR 0045): the update check contacts api.github.com, so it must
    // not run before the user has explicitly opted in — matching the "no network
    // connections by default" promise in PRIVACY.md / product-spec §13-14. The
    // user can turn this on from the Settings update card.
    bool check_updates_on_start = false;

    // WHATS-NEW: suppress the one-time post-update "What's new" overlay on future
    // updates. Default false (notices ARE shown). This only affects the post-update
    // auto-show; the Settings update-card "What's new" link is never suppressed.
    bool whats_new_suppressed = false;

    // Process-handoff guard for the staged updater. Set only when the updater's
    // marked close request is accepted (never on mere process launch), and
    // discarded by every new app process. Verification reinstalls never write it.
    // Empty = no committed handoff. Default empty.
    QString applied_version;

    // Appearance and accent are two independent choices. They replace the single
    // `theme_id` that named one of four complete themes; a stored `theme_id` is
    // migrated to the closest pair on load and then never written again, so an
    // existing install keeps the colour it had rather than snapping to the
    // default. See ui/theme/ExoSnapThemes.h for the mapping.
    QString appearance_id = QStringLiteral("dark");
    QString accent_id = QStringLiteral("aqua");

    // SETTINGS-TIERS-R1: global Expert mode toggle (default OFF).
    bool expert_mode_enabled = false;

    // SETTINGS-TIERS-R1: per-card expander expanded state (default collapsed).
    bool audio_separate_expander_expanded = false;

    // ELEVATION-FOUNDATION-R1 (ADR 0033): opt-in for elevation-gated present /
    // tearing diagnostics (PresentMon ETW). Default OFF. The later PresentMon
    // slice gates its provider on this flag AND the runtime elevation state;
    // turning it on while non-elevated offers the "relaunch as administrator"
    // path. Persisted so the choice survives the self-relaunch.
    bool present_diagnostics_optin = false;

    // SETTINGS-HONESTY-R1: developer log-level filter (Settings > Advanced >
    // Developer card, expert-only). One of "Off" | "Error" | "Warning" | "Info" |
    // "Debug" -- see AppLog::setMinSeverity. Ship default is "Debug" (record
    // everything, review F1): main recorded every severity before this control was
    // wired, and Debug lines (DxgiPreviewRenderer, target enumeration, ...) are
    // exactly what support cases need. The filter only narrows on explicit user choice.
    QString developer_log_level = QStringLiteral("Debug");

    // Transient — not written by Save(). Says which of the three load cases
    // produced the fields above; see SettingsLoadOutcome. Defaults to
    // DefaultsNoFile so a default-constructed struct (tests, harness scenarios)
    // never claims to be a faithful read of a file.
    SettingsLoadOutcome load_outcome = SettingsLoadOutcome::DefaultsNoFile;
};

class AppSettingsStore {
  public:
    AppSettingsStore();
    explicit AppSettingsStore(QString settings_file_path);

    [[nodiscard]] PersistedAppSettings Load() const;

    // Returns false when the settings could not be persisted: no path is
    // configured, or QSettings reported an error after sync(). The result is
    // [[nodiscard]] on purpose — ignoring it is how a failed write used to
    // become an invisible loss of the user's change.
    [[nodiscard]] bool Save(const PersistedAppSettings& settings) const;

    // Move an existing settings file aside to "<settings.ini>.corrupt" so that
    // an unreadable file is preserved rather than overwritten. Called exactly
    // once, immediately before the first write that follows a
    // SettingsLoadOutcome::ReadFailed load. Any earlier backup is replaced —
    // one unreadable file is kept, not a growing pile. Returns true when a
    // backup was made and writes its path to `out_backup_path` if given.
    bool BackupUnreadableFile(QString* out_backup_path = nullptr) const;

    [[nodiscard]] const QString& SettingsFilePath() const;

  private:
    QString settings_path_;
};

} // namespace exosnap
