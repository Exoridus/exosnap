#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace exosnap::settings {

namespace internal {
inline QString portable_data_dir;
}

// Reads the optional portable marker before opening stores. Returns a startup
// error if the marker is present but its configured path cannot be used.
[[nodiscard]] QString InitializePortableDataDir(const QString& executable_dir);

[[nodiscard]] inline QString ResolvePersistenceOverride() {
    const QString override_dir = qEnvironmentVariable("EXOSNAP_CONFIG_DIR");
    return override_dir.isEmpty() ? internal::portable_data_dir : override_dir;
}

inline QString ResolveAppConfigDir() {
    const QString root = ResolvePersistenceOverride();
    return root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) : root;
}

inline QString ResolveAppDataDir() {
    const QString root = ResolvePersistenceOverride();
    return root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : root;
}

inline QString ResolveAppCacheDir() {
    const QString root = ResolvePersistenceOverride();
    return root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                          : QDir(root).filePath(QStringLiteral("cache"));
}

} // namespace exosnap::settings
