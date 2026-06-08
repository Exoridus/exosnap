#pragma once

#include "../models/OutputSettingsModel.h"
#include "../models/RecordingProfile.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include <capability/audio_ui_state.h>

#include <QString>
#include <array>
#include <vector>

namespace exosnap {

struct PersistedWindowGeometry {
    int x = -1;
    int y = -1;
    int width = -1;
    int height = -1;
    bool maximized = false;
};

struct PersistedAppSettings {
    OutputSettingsModel output;
    VideoSettingsModel video;
    capability::AudioUiState audio_ui_state;
    WebcamSettings webcam;
    std::vector<RecordingProfile> user_profiles;
    std::vector<RecordingProfile> modified_builtin_profiles;
    ActiveRecordingProfileState active_profile;
    std::array<QString, 4> hotkey_bindings = {
        QStringLiteral("Alt+F9"),
        QString(),
        QString(),
        QString(),
    };
    PersistedWindowGeometry window_geometry;
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
