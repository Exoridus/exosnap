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
