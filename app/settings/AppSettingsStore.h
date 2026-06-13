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
