#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AtomicFileOps.h"

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "settings/ConfigPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QString>
#include <QUuid>
#include <mutex>

namespace {
std::mutex publication_mutex;
constexpr qint64 kJournalByteLimit = 64 * 1024;

QString PublicationDirectory() {
    const auto root = exosnap::settings::ResolveAppConfigDir();
    return root.isEmpty() ? QString{} : QDir(root).filePath(QStringLiteral("publications"));
}

DWORD FileIdentity(const std::filesystem::path& path, QString& identity) {
    identity.clear();
    HANDLE file =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool read = GetFileInformationByHandle(file, &info) != 0;
    const auto error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (error)
        return error;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return ERROR_ACCESS_DENIED;
    identity = QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
                   .arg(info.dwVolumeSerialNumber)
                   .arg(info.nFileIndexHigh)
                   .arg(info.nFileIndexLow)
                   .arg(info.nFileSizeHigh)
                   .arg(info.nFileSizeLow)
                   .arg(info.ftLastWriteTime.dwHighDateTime)
                   .arg(info.ftLastWriteTime.dwLowDateTime);
    return ERROR_SUCCESS;
}

struct PublicationFile {
    std::filesystem::path source;
    std::filesystem::path target;
    std::filesystem::path backup;
    QString fresh;
    QString previous;
};

QJsonObject SerializePublicationFile(const PublicationFile& file) {
    return {{QStringLiteral("source"), QString::fromStdWString(file.source.wstring())},
            {QStringLiteral("target"), QString::fromStdWString(file.target.wstring())},
            {QStringLiteral("backup"), QString::fromStdWString(file.backup.wstring())},
            {QStringLiteral("fresh"), file.fresh},
            {QStringLiteral("previous"), file.previous}};
}

DWORD RestorePublication(const std::vector<PublicationFile>& files) {
    if (files.empty())
        return ERROR_INVALID_DATA;
    QString source_media, target_media;
    if (const auto error = FileIdentity(files.front().source, source_media))
        return error;
    if (const auto error = FileIdentity(files.front().target, target_media))
        return error;
    const bool committed = source_media.isEmpty() && target_media == files.front().fresh;
    if (!committed && (source_media != files.front().fresh || target_media != files.front().previous))
        return ERROR_FILE_INVALID;
    // Check every object before changing any path. A user edit makes the journal
    // ambiguous and must never be replaced by an older transaction's bytes.
    for (size_t index = 1; index < files.size(); ++index) {
        const auto& file = files[index];
        QString source, target, backup;
        if (const auto error = FileIdentity(file.source, source))
            return error;
        if (const auto error = FileIdentity(file.target, target))
            return error;
        if (!file.backup.empty())
            if (const auto error = FileIdentity(file.backup, backup))
                return error;
        if ((!source.isEmpty() && source != file.fresh) ||
            (!target.isEmpty() && target != file.fresh && target != file.previous) ||
            (!backup.isEmpty() && backup != file.previous))
            return ERROR_FILE_INVALID;
        if (committed && (!source.isEmpty() || target != file.fresh))
            return ERROR_FILE_INVALID;
        if (!committed && !file.fresh.isEmpty() && source.isEmpty() && target != file.fresh)
            return ERROR_FILE_INVALID;
        if (!committed && !file.previous.isEmpty() && target != file.previous && backup != file.previous)
            return ERROR_FILE_INVALID;
    }
    for (size_t index = files.size(); index-- > 1;) {
        const auto& file = files[index];
        QString source, target, backup, current_media;
        if (const auto error = FileIdentity(files.front().target, current_media))
            return error;
        if (current_media != (committed ? files.front().fresh : files.front().previous))
            return ERROR_FILE_INVALID;
        if (const auto error = FileIdentity(file.source, source))
            return error;
        if (const auto error = FileIdentity(file.target, target))
            return error;
        if (!file.backup.empty())
            if (const auto error = FileIdentity(file.backup, backup))
                return error;
        if ((!source.isEmpty() && source != file.fresh) ||
            (!target.isEmpty() && target != file.fresh && target != file.previous) ||
            (!backup.isEmpty() && backup != file.previous))
            return ERROR_FILE_INVALID;
        if (committed && (!source.isEmpty() || target != file.fresh))
            return ERROR_FILE_INVALID;
        if (!committed) {
            if (!file.fresh.isEmpty() && target == file.fresh) {
                if (const auto error = exosnap::AtomicReplaceInPlace(file.target, file.source))
                    return error;
            }
            if (!backup.isEmpty())
                if (const auto error = exosnap::AtomicReplaceInPlace(file.backup, file.target))
                    return error;
        } else if (!backup.isEmpty() && !DeleteFileW(file.backup.c_str())) {
            return GetLastError();
        }
    }
    return ERROR_SUCCESS;
}

bool LoadPublication(const QString& journal, std::vector<PublicationFile>& files) {
    QFile input(journal);
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 || input.size() > kJournalByteLimit)
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto object = document.object();
    if (object.value(QStringLiteral("version")).toInt() != 1)
        return false;
    const auto array = object.value(QStringLiteral("files")).toArray();
    if (array.isEmpty() || array.size() > 3)
        return false;
    for (const auto& item : array) {
        if (!item.isObject())
            return false;
        const auto record = item.toObject();
        PublicationFile file;
        file.source = record.value(QStringLiteral("source")).toString().toStdWString();
        file.target = record.value(QStringLiteral("target")).toString().toStdWString();
        file.backup = record.value(QStringLiteral("backup")).toString().toStdWString();
        file.fresh = record.value(QStringLiteral("fresh")).toString();
        file.previous = record.value(QStringLiteral("previous")).toString();
        if (!file.source.is_absolute() || !file.target.is_absolute() || exosnap::PathsEqual(file.source, file.target))
            return false;
        files.push_back(std::move(file));
    }
    const auto& media = files.front();
    if (media.fresh.isEmpty() || !media.backup.empty())
        return false;
    for (size_t index = 1; index < files.size(); ++index) {
        const auto& file = files[index];
        auto source_markers = media.source;
        auto target_markers = media.target;
        source_markers.replace_extension(L".markers.json");
        target_markers.replace_extension(L".markers.json");
        const bool identity =
            exosnap::PathsEqual(file.source, std::filesystem::path(media.source.wstring() + L".exosnap.json")) &&
            exosnap::PathsEqual(file.target, std::filesystem::path(media.target.wstring() + L".exosnap.json"));
        const bool markers =
            exosnap::PathsEqual(file.source, source_markers) && exosnap::PathsEqual(file.target, target_markers);
        if (!identity && !markers)
            return false;
        if (!file.previous.isEmpty()) {
            const auto expected = std::filesystem::path(file.target.wstring() + L".publication-" +
                                                        QFileInfo(journal).completeBaseName().toStdWString() + L".bak");
            if (!exosnap::PathsEqual(file.backup, expected))
                return false;
        } else if (!file.backup.empty()) {
            return false;
        }
    }
    return true;
}
} // namespace

namespace exosnap {

bool PathsEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto norm = [](std::filesystem::path p) {
        std::wstring s = p.lexically_normal().generic_wstring();
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
        return s;
    };
    return norm(a) == norm(b);
}

std::filesystem::path MakeDisposableSiblingStagingPath(const std::filesystem::path& target) {
    const std::filesystem::path dir = target.parent_path();
    const std::wstring base = target.filename().wstring();
    for (int n = 0; n < 100000; ++n) {
        const std::wstring suffix = n == 0 ? L".tmp" : L"." + std::to_wstring(n) + L".tmp";
        const std::filesystem::path candidate = dir / (base + suffix);
        const auto identity = std::filesystem::path(candidate.wstring() + L".exosnap.json");
        auto markers = candidate;
        markers.replace_extension(L".markers.json");
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec && !std::filesystem::exists(identity, ec) && !ec &&
            !std::filesystem::exists(markers, ec) && !ec)
            return candidate;
    }
    return dir / (base + L".tmp");
}

std::filesystem::path MakeSiblingTempPath(const std::filesystem::path& target) {
    return MakeDisposableSiblingStagingPath(target);
}

unsigned long AtomicReplaceInPlace(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return 0;
    return ::GetLastError();
}

unsigned long StageCompletedMediaForPublication(const std::filesystem::path& source,
                                                const std::filesystem::path& staging) {
    if (!CreateHardLinkW(staging.c_str(), source.c_str(), nullptr))
        return GetLastError();
    const auto source_sidecar = std::filesystem::path(source.wstring() + L".exosnap.json");
    const auto staged_sidecar = std::filesystem::path(staging.wstring() + L".exosnap.json");
    const auto attributes = GetFileAttributesW(source_sidecar.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return ERROR_SUCCESS;
        DeleteFileW(staging.c_str());
        return error;
    }
    if (!CopyFileW(source_sidecar.c_str(), staged_sidecar.c_str(), TRUE)) {
        const auto error = GetLastError();
        DeleteFileW(staging.c_str());
        return error;
    }
    return ERROR_SUCCESS;
}

unsigned long PublishMediaAndIdentitySidecar(const std::filesystem::path& from, const std::filesystem::path& to,
                                             bool replace_existing, std::function<void()> rename_committed) {
    if (from.native().size() > 4096 || to.native().size() > 4096)
        return ERROR_BUFFER_OVERFLOW;
    if (PathsEqual(from, to))
        return ERROR_INVALID_PARAMETER;
    std::lock_guard guard(publication_mutex);
    const auto directory = PublicationDirectory();
    if (directory.isEmpty() || !QDir().mkpath(directory))
        return ERROR_PATH_NOT_FOUND;
    QLockFile lock(QDir(directory).filePath(QStringLiteral("active.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(30000))
        return ERROR_BUSY;
    std::error_code path_error;
    const auto source = std::filesystem::absolute(from, path_error);
    if (path_error)
        return ERROR_INVALID_NAME;
    const auto target = std::filesystem::absolute(to, path_error);
    if (path_error)
        return ERROR_INVALID_NAME;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::vector<PublicationFile> files{{source, target}};
    if (const auto error = FileIdentity(source, files.front().fresh))
        return error;
    if (files.front().fresh.isEmpty())
        return ERROR_FILE_NOT_FOUND;
    if (const auto error = FileIdentity(target, files.front().previous))
        return error;
    if (!replace_existing && !files.front().previous.isEmpty())
        return ERROR_FILE_EXISTS;
    auto source_markers = source;
    auto target_markers = target;
    source_markers.replace_extension(L".markers.json");
    target_markers.replace_extension(L".markers.json");
    const std::pair<std::filesystem::path, std::filesystem::path> candidates[] = {
        {std::filesystem::path(source.wstring() + L".exosnap.json"),
         std::filesystem::path(target.wstring() + L".exosnap.json")},
        {source_markers, target_markers}};
    for (const auto& [staged, final] : candidates) {
        PublicationFile file{staged, final};
        if (const auto error = FileIdentity(staged, file.fresh))
            return error;
        if (const auto error = FileIdentity(final, file.previous))
            return error;
        if (file.fresh.isEmpty() && file.previous.isEmpty())
            continue;
        if (!file.previous.isEmpty()) {
            if (!replace_existing)
                return ERROR_FILE_EXISTS;
            file.backup = std::filesystem::path(final.wstring() + L".publication-" + id.toStdWString() + L".bak");
        }
        files.push_back(std::move(file));
    }
    QJsonArray records;
    for (const auto& file : files)
        records.append(SerializePublicationFile(file));
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("files"), records}})
                           .toJson(QJsonDocument::Compact);
    if (bytes.size() > kJournalByteLimit)
        return ERROR_BUFFER_OVERFLOW;
    const auto journal = QDir(directory).filePath(id + QStringLiteral(".json"));
    QSaveFile output(journal);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return ERROR_WRITE_FAULT;
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    const auto rename = [&](const std::filesystem::path& staged, const std::filesystem::path& final,
                            DWORD rename_flags) -> DWORD {
        if (!MoveFileExW(staged.c_str(), final.c_str(), rename_flags))
            return GetLastError();
        if (rename_committed)
            rename_committed();
        return ERROR_SUCCESS;
    };
    const auto rollback = [&](DWORD error) {
        const auto restore = RestorePublication(files);
        if (!restore)
            QFile::remove(journal);
        return restore ? restore : error;
    };
    for (size_t index = 1; index < files.size(); ++index) {
        const auto& file = files[index];
        if (!file.previous.isEmpty())
            if (const auto error = rename(file.target, file.backup, MOVEFILE_WRITE_THROUGH))
                return rollback(error);
        if (!file.fresh.isEmpty())
            if (const auto error = rename(file.source, file.target, flags))
                return rollback(error);
    }
    // The durable record exists before every rename. Media is the commit point;
    // its identity also distinguishes a crash before commit from one after it.
    if (const auto error = rename(source, target, flags))
        return rollback(error);
    if (!RestorePublication(files))
        QFile::remove(journal);
    return ERROR_SUCCESS;
}

std::string RecoverInterruptedMediaPublications() {
    std::lock_guard guard(publication_mutex);
    const auto directory = PublicationDirectory();
    if (directory.isEmpty())
        return "Publication recovery configuration directory is unavailable";
    if (!QDir(directory).exists())
        return {};
    QLockFile lock(QDir(directory).filePath(QStringLiteral("active.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return "Publication recovery is waiting for an active publisher";
    std::string failures;
    const auto journals = QDir(directory).entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const auto& name : journals) {
        const auto journal = QDir(directory).filePath(name);
        std::vector<PublicationFile> files;
        const auto error = LoadPublication(journal, files) ? RestorePublication(files) : ERROR_INVALID_DATA;
        if (!error && QFile::remove(journal))
            continue;
        if (!failures.empty())
            failures += "\n";
        failures += "Publication transaction retained: " + name.toStdString() + " (Win32 error " +
                    std::to_string(error ? error : ERROR_ACCESS_DENIED) + ")";
    }
    return failures;
}

std::string DescribeFailedStagingRemoval(const std::filesystem::path& staging) {
    std::error_code path_error;
    const auto absolute_staging = std::filesystem::absolute(staging, path_error);
    if (path_error)
        return "Staging retained because its absolute path cannot be resolved";
    {
        std::lock_guard guard(publication_mutex);
        const auto directory = PublicationDirectory();
        if (!directory.isEmpty() && QDir(directory).exists()) {
            QLockFile lock(QDir(directory).filePath(QStringLiteral("active.lock")));
            lock.setStaleLockTime(0);
            if (!lock.tryLock(0))
                return "Staging retained while publication recovery is active";
            for (const auto& name : QDir(directory).entryList({QStringLiteral("*.json")}, QDir::Files)) {
                std::vector<PublicationFile> files;
                if (!LoadPublication(QDir(directory).filePath(name), files))
                    return "Staging retained because a publication recovery record is invalid";
                if (PathsEqual(files.front().source, absolute_staging))
                    return "Staging retained for interrupted publication recovery";
            }
        }
    }
    std::string failures;
    auto markers = staging;
    markers.replace_extension(L".markers.json");
    const std::filesystem::path paths[] = {staging, std::filesystem::path(staging.wstring() + L".exosnap.json"),
                                           markers};
    for (const auto& path : paths) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!ec)
            continue;
        std::error_code exists_ec;
        if (!std::filesystem::exists(path, exists_ec) && !exists_ec)
            continue;
        if (!failures.empty())
            failures += "\n";
        failures += "could not remove the staging file \"" + path.filename().string() + "\": " + ec.message() +
                    "; it is still on disk next to the recording";
    }
    return failures;
}

} // namespace exosnap
