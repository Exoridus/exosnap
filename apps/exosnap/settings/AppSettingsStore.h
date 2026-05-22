#pragma once

#include "../models/OutputSettingsModel.h"
#include <capability/audio_ui_state.h>

#include <QString>

namespace exosnap {

struct PersistedAppSettings {
    OutputSettingsModel output;
    capability::AudioUiState audio_ui_state;
};

class AppSettingsStore {
  public:
    AppSettingsStore();
    explicit AppSettingsStore(QString settings_file_path);

    [[nodiscard]] PersistedAppSettings Load() const;
    void Save(const PersistedAppSettings& settings) const;

    [[nodiscard]] QString SettingsFilePath() const;

  private:
    QString settings_path_;
};

} // namespace exosnap
