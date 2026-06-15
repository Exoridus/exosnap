#include "AppSettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "settings/ConfigPaths.h"

namespace exosnap {
namespace {

// Bump to 14: ACCENT-PICKER-R1 adds accent_id (default "mint").
// Pre-1.0: no migration; incompatible persisted data is reset to defaults.
constexpr int kSettingsVersionCurrent = 14;

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
    // DIAGNOSTICS-OVERLAY-R1: diagnostics overlay toggle (default OFF).
    // Pre-1.0: no migration; missing key defaults to false.
    persisted.show_diagnostics_overlay = settings.value(QStringLiteral("show_diagnostics_overlay"), false).toBool();
    // NOTIFY-TOASTS-R1: notification toasts toggle (default ON).
    // Pre-1.0: no migration; missing key defaults to true.
    persisted.show_notifications = settings.value(QStringLiteral("show_notifications"), true).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("tray"));
    // TRAY-CLOSE-TO-TRAY-R1: close-to-tray opt-in (default OFF).
    persisted.keep_running_in_tray = settings.value(QStringLiteral("keep_running_in_tray"), false).toBool();
    // TRAY-CLOSE-TO-TRAY-R1: one-time close notice shown flag (default false).
    persisted.tray_close_notice_shown = settings.value(QStringLiteral("tray_close_notice_shown"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("presence"));
    // QUICK-PILL-R1: interactive quick-control pill toggle (default OFF).
    // Pre-1.0: no migration; missing key defaults to false.
    persisted.show_quick_controls = settings.value(QStringLiteral("show_quick_controls"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("crash"));
    // CRASH-WIRE-R1: auto-send opt-in (default OFF).
    // Pre-1.0: no migration; missing key defaults to false.
    persisted.auto_send_crash_reports = settings.value(QStringLiteral("auto_send_crash_reports"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("update"));
    // UPDATE-WIRE-R1: update channel (default "Stable") + auto-check-on-start (default true).
    // Pre-1.0: no migration; missing keys default to Stable / true.
    persisted.update_channel = settings.value(QStringLiteral("channel"), QStringLiteral("Stable")).toString();
    persisted.check_updates_on_start = settings.value(QStringLiteral("check_updates_on_start"), true).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("appearance"));
    // ACCENT-PICKER-R1: accent color id (default "mint" = Studio Mint).
    // Pre-1.0: no migration; missing key defaults to "mint".
    persisted.accent_id = settings.value(QStringLiteral("accent_id"), QStringLiteral("mint")).toString();
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
    // DIAGNOSTICS-OVERLAY-R1: diagnostics overlay toggle.
    settings.setValue(QStringLiteral("show_diagnostics_overlay"), settings_snapshot.show_diagnostics_overlay);
    // NOTIFY-TOASTS-R1: notification toasts toggle.
    settings.setValue(QStringLiteral("show_notifications"), settings_snapshot.show_notifications);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("tray"));
    // TRAY-CLOSE-TO-TRAY-R1: close-to-tray opt-in.
    settings.setValue(QStringLiteral("keep_running_in_tray"), settings_snapshot.keep_running_in_tray);
    // TRAY-CLOSE-TO-TRAY-R1: one-time close notice shown flag.
    settings.setValue(QStringLiteral("tray_close_notice_shown"), settings_snapshot.tray_close_notice_shown);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("presence"));
    // QUICK-PILL-R1: interactive quick-control pill toggle.
    settings.setValue(QStringLiteral("show_quick_controls"), settings_snapshot.show_quick_controls);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("crash"));
    // CRASH-WIRE-R1: auto-send opt-in.
    settings.setValue(QStringLiteral("auto_send_crash_reports"), settings_snapshot.auto_send_crash_reports);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("update"));
    // UPDATE-WIRE-R1: update channel + auto-check-on-start.
    settings.setValue(QStringLiteral("channel"), settings_snapshot.update_channel);
    settings.setValue(QStringLiteral("check_updates_on_start"), settings_snapshot.check_updates_on_start);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("appearance"));
    // ACCENT-PICKER-R1: accent color id.
    settings.setValue(QStringLiteral("accent_id"), settings_snapshot.accent_id);
    settings.endGroup();

    settings.sync();
}

const QString& AppSettingsStore::SettingsFilePath() const {
    return settings_path_;
}

} // namespace exosnap
