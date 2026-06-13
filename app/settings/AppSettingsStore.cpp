#include "AppSettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "settings/ConfigPaths.h"

namespace exosnap {
namespace {

// Bump to 8: diagnostics overlay toggle added.
constexpr int kSettingsVersionCurrent = 8;

} // namespace

AppSettingsStore::AppSettingsStore() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        settings_path_ = QDir(config_dir).filePath(QStringLiteral("settings.ini"));
    } else {
        settings_path_ = QStringLiteral("settings.ini");
    }
}

AppSettingsStore::AppSettingsStore(QString settings_file_path) : settings_path_(std::move(settings_file_path)) {
}

PersistedAppSettings AppSettingsStore::Load() const {
    PersistedAppSettings persisted;

    if (settings_path_.isEmpty()) {
        return persisted;
    }

    QSettings settings(settings_path_, QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("hotkeys"));
    for (int i = 0; i < static_cast<int>(persisted.hotkey_bindings.size()); ++i) {
        const QString key = QStringLiteral("binding_%1").arg(i);
        if (settings.contains(key)) {
            persisted.hotkey_bindings[static_cast<std::size_t>(i)] = settings.value(key).toString().trimmed();
        }
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("window"));
    persisted.window_geometry.x = settings.value(QStringLiteral("x"), -1).toInt();
    persisted.window_geometry.y = settings.value(QStringLiteral("y"), -1).toInt();
    persisted.window_geometry.width = settings.value(QStringLiteral("width"), -1).toInt();
    persisted.window_geometry.height = settings.value(QStringLiteral("height"), -1).toInt();
    persisted.window_geometry.maximized = settings.value(QStringLiteral("maximized"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("overlay"));
    persisted.show_recording_overlay = settings.value(QStringLiteral("show_recording_overlay"), true).toBool();
    persisted.show_diagnostics_overlay = settings.value(QStringLiteral("show_diagnostics_overlay"), false).toBool();
    settings.endGroup();

    return persisted;
}

void AppSettingsStore::Save(const PersistedAppSettings& settings_snapshot) const {
    if (settings_path_.isEmpty()) {
        return;
    }

    const QFileInfo info(settings_path_);
    QDir().mkpath(info.absolutePath());

    QSettings settings(settings_path_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), kSettingsVersionCurrent);

    // Remove legacy groups from old builds so files are fully canonical after
    // one Save() call.  These keys are now owned by RecordingPresetStore.
    settings.remove(QStringLiteral("output"));
    settings.remove(QStringLiteral("video"));
    settings.remove(QStringLiteral("audio"));
    settings.remove(QStringLiteral("webcam"));
    settings.remove(QStringLiteral("profiles"));

    settings.beginGroup(QStringLiteral("hotkeys"));
    for (int i = 0; i < static_cast<int>(settings_snapshot.hotkey_bindings.size()); ++i) {
        settings.setValue(QStringLiteral("binding_%1").arg(i),
                          settings_snapshot.hotkey_bindings[static_cast<std::size_t>(i)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("window"));
    settings.setValue(QStringLiteral("x"), settings_snapshot.window_geometry.x);
    settings.setValue(QStringLiteral("y"), settings_snapshot.window_geometry.y);
    settings.setValue(QStringLiteral("width"), settings_snapshot.window_geometry.width);
    settings.setValue(QStringLiteral("height"), settings_snapshot.window_geometry.height);
    settings.setValue(QStringLiteral("maximized"), settings_snapshot.window_geometry.maximized);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("overlay"));
    settings.setValue(QStringLiteral("show_recording_overlay"), settings_snapshot.show_recording_overlay);
    settings.setValue(QStringLiteral("show_diagnostics_overlay"), settings_snapshot.show_diagnostics_overlay);
    settings.endGroup();

    settings.sync();
}

const QString& AppSettingsStore::SettingsFilePath() const {
    return settings_path_;
}

} // namespace exosnap
