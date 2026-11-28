#include "settings/ConfigPaths.h"

#include <update/install_mode_detector.h>
#include <update/runtime_swap_lock.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#include <toml++/toml.hpp>

#include <filesystem>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap::settings {
namespace {

bool IsWithin(const QString& path, const QString& parent) {
    return path.compare(parent, Qt::CaseInsensitive) == 0 ||
           path.startsWith(parent + QLatin1Char('/'), Qt::CaseInsensitive);
}

bool IsRuntimeUpdatePath(const QString& path, const QString& runtime) {
    const QDir root = QFileInfo(runtime).dir();
    return IsWithin(path, runtime) || IsWithin(path, runtime + QStringLiteral(".new")) ||
           IsWithin(path, runtime + QStringLiteral(".old")) || IsWithin(path, runtime + QStringLiteral(".new.hoist")) ||
           IsWithin(path, root.filePath(QString::fromWCharArray(update::kRuntimeSwapLockFile)));
}

QString DataPathError(const QString& path) {
    return QCoreApplication::translate("PortableConfig", "Cannot use the portable data folder: %1").arg(path);
}

} // namespace

QString InitializePortableDataDir(const QString& executable_dir) {
    internal::portable_data_dir.clear();
    if (!qEnvironmentVariable("EXOSNAP_CONFIG_DIR").isEmpty())
        return {};

    const auto root = QString::fromStdWString(update::PackageRootForExecutableDirectory(executable_dir.toStdWString()));
    if (root == executable_dir)
        return {};
    const QDir application_root(root);
    QFile marker(application_root.filePath(QStringLiteral("portable.toml")));
    const auto marker_path = QDir::toNativeSeparators(marker.fileName()).toStdWString();
    if (GetFileAttributesW(marker_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return QCoreApplication::translate("PortableConfig", "Cannot read portable.toml.");
    }
    const QString invalid_marker =
        QCoreApplication::translate("PortableConfig", "portable.toml must contain a valid data_path.");
    if (!marker.open(QIODevice::ReadOnly))
        return QCoreApplication::translate("PortableConfig", "Cannot read portable.toml.");

    QString configured;
    try {
        const QByteArray bytes = marker.readAll();
        const auto document = toml::parse(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        const auto value = document["data_path"].value<std::string_view>();
        if (!value || value->empty() || value->find('\0') != std::string_view::npos)
            return invalid_marker;
        configured = QString::fromUtf8(value->data(), static_cast<qsizetype>(value->size()));
    } catch (const toml::parse_error&) {
        return invalid_marker;
    }

    const std::filesystem::path path(configured.toStdWString());
    if (!path.is_absolute() && (path.has_root_name() || path.has_root_directory()))
        return invalid_marker;
    const QString resolved =
        QDir::cleanPath(path.is_absolute() ? configured : application_root.absoluteFilePath(configured));
    if (IsRuntimeUpdatePath(resolved, QDir(executable_dir).absolutePath()))
        return DataPathError(resolved);
    if (!QDir().mkpath(resolved))
        return DataPathError(resolved);

    const auto canonical = QFileInfo(resolved).canonicalFilePath();
    const auto canonical_runtime = QFileInfo(executable_dir).canonicalFilePath();
    if (canonical.isEmpty() || canonical_runtime.isEmpty() || IsRuntimeUpdatePath(canonical, canonical_runtime))
        return DataPathError(resolved);

    QTemporaryFile probe(QDir(resolved).filePath(QStringLiteral(".exosnap-write-test-XXXXXX")));
    if (!probe.open())
        return DataPathError(resolved);

    internal::portable_data_dir = resolved;
    return {};
}

} // namespace exosnap::settings
