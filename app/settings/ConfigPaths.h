#pragma once

#include <QStandardPaths>
#include <QString>

namespace exosnap::settings {

// Optional persistence-root override.
//
// When the EXOSNAP_CONFIG_DIR environment variable is set to a non-empty value,
// all persisted application data — settings.ini, presets.ini,
// recording-history.json, and the diagnostics log — resolves under that
// directory instead of the per-user Windows standard locations.
//
// Default behavior (variable unset) is unchanged: the standard
// AppConfigLocation / AppLocalDataLocation are used. The override exists so a
// run can be fully isolated from real user data — used by the release smoke
// test and CI so launching the shipped executable cannot touch a real user's
// settings, presets, or recording history.
inline QString ResolveAppConfigDir() {
    const QString override_dir = qEnvironmentVariable("EXOSNAP_CONFIG_DIR");
    if (!override_dir.isEmpty())
        return override_dir;
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

inline QString ResolveAppDataDir() {
    const QString override_dir = qEnvironmentVariable("EXOSNAP_CONFIG_DIR");
    if (!override_dir.isEmpty())
        return override_dir;
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

} // namespace exosnap::settings
