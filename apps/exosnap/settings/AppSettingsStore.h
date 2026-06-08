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
