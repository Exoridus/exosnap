#pragma once

#include <QString>
#include <array>
#include <string>

namespace exosnap {

struct PersistedWindowGeometry {
    int x = -1;
    int y = -1;
    int width = -1;
    int height = -1;
    bool maximized = false;
};

struct PersistedAppSettings {
    // Indexed by HotkeyAction: ToggleRecording, TogglePause, CaptureFrame,
    // AddMarker, SplitRecording. Size must match kHotkeyActionCount.
    std::array<QString, 5> hotkey_bindings = {
        QStringLiteral("Alt+F9"), QString(), QString(), QString(), QString(),
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

    // NOTIFY-TOASTS-R1: whether transient notification toasts are shown.
    // Default ON. Excluded from capture via SetWindowDisplayAffinity.
    // Covers: LowStorage, Saved, UnexpectedStop, RecoveryAvailable.
    bool show_notifications = true;

    // "Open editor when finished" (Output card): when a recording completes
    // successfully, open the Edit overlay directly instead of showing the
    // "Recording saved" toast with Edit/Show-in-folder actions. Default OFF
    // (unchanged toast-based behavior). Was previously an ADR-0031 debug-only
    // roadmap-dummy toggle with no engine setting behind it at all.
    bool open_editor_when_finished = false;

    // TRAY-CLOSE-TO-TRAY-R1: whether the window hides to the tray instead of
    // quitting when the user clicks the window's close button. Default OFF.
    bool keep_running_in_tray = false;

    // TRAY-CLOSE-TO-TRAY-R1: whether the one-time close-to-tray notice has
    // already been shown to the user. Default false (not yet shown).
    bool tray_close_notice_shown = false;

    // QUICK-PILL-R1: whether the interactive quick-control pill overlay is shown
    // during recording. Default OFF.  The pill is capture-excluded (via
    // SetWindowDisplayAffinity) and interactive (NOT click-through), so it is an
    // opt-in feature gated here.
    bool show_quick_controls = false;

    // CRASH-WIRE-R1 (ADR 0017): when true, the next-launch crash dialog is
    // suppressed and consent is granted silently so the (dormant w/o DSN) report
    // is sent automatically. Opt-in only; default OFF.
    bool auto_send_crash_reports = false;

    // UPDATE-WIRE-R1 (ADR 0012): the selected update channel — "Stable" | "Preview".
    // Applied immediately on change (persist + re-check); default Stable.
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

    // THEME-SLICE-1: accent_id renamed to theme_id. Pre-1.0: stale accent_id key in
    // persisted data is simply ignored.
    QString theme_id = QStringLiteral("dark-default");

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

    // Transient — not written by Save(). False when settings.ini existed but
    // QSettings::status() reported an error while reading it (corrupt/locked
    // file); every field above is then the built-in default rather than a
    // faithful read of whatever was on disk. True on a normal load, including
    // a missing file (first run).
    bool load_ok = true;
};

class AppSettingsStore {
  public:
    AppSettingsStore();
    explicit AppSettingsStore(QString settings_file_path);

    [[nodiscard]] PersistedAppSettings Load() const;
    void Save(const PersistedAppSettings& settings) const;

    [[nodiscard]] const QString& SettingsFilePath() const;

  private:
    QString settings_path_;
};

} // namespace exosnap
