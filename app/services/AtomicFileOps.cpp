#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AtomicFileOps.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

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
                                             bool replace_existing) {
    if (PathsEqual(from, to))
        return ERROR_INVALID_PARAMETER;
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    const auto publish = [flags](const std::filesystem::path& source, const std::filesystem::path& target) -> DWORD {
        return MoveFileExW(source.c_str(), target.c_str(), flags) ? ERROR_SUCCESS : GetLastError();
    };
    if (!replace_existing) {
        if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES)
            return ERROR_FILE_EXISTS;
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return error;
    }
    struct Companion {
        std::filesystem::path source;
        std::filesystem::path target;
        std::filesystem::path backup;
        bool existed = false;
        bool backed_up = false;
        bool published = false;
    };
    auto from_markers = from;
    auto to_markers = to;
    from_markers.replace_extension(L".markers.json");
    to_markers.replace_extension(L".markers.json");
    std::vector<Companion> companions;
    companions.push_back({std::filesystem::path(from.wstring() + L".exosnap.json"),
                          std::filesystem::path(to.wstring() + L".exosnap.json")});
    companions.push_back({from_markers, to_markers});
    for (auto iterator = companions.begin(); iterator != companions.end();) {
        const auto attributes = GetFileAttributesW(iterator->source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                return error;
            iterator = companions.erase(iterator);
            continue;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            return ERROR_ACCESS_DENIED;
        const auto target_attributes = GetFileAttributesW(iterator->target.c_str());
        iterator->existed = target_attributes != INVALID_FILE_ATTRIBUTES;
        if (iterator->existed) {
            if (!replace_existing)
                return ERROR_FILE_EXISTS;
            if (target_attributes & FILE_ATTRIBUTE_DIRECTORY)
                return ERROR_ACCESS_DENIED;
            iterator->backup = MakeDisposableSiblingStagingPath(iterator->target);
        } else {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                return error;
        }
        ++iterator;
    }
    const auto rollback = [&](DWORD original_error) {
        DWORD restore_error = ERROR_SUCCESS;
        for (auto iterator = companions.rbegin(); iterator != companions.rend(); ++iterator) {
            if (iterator->published) {
                const auto error = AtomicReplaceInPlace(iterator->target, iterator->source);
                if (error && !restore_error)
                    restore_error = error;
            }
            if (iterator->backed_up) {
                const auto error = AtomicReplaceInPlace(iterator->backup, iterator->target);
                if (error && !restore_error)
                    restore_error = error;
            }
        }
        return restore_error ? restore_error : original_error;
    };
    for (auto& companion : companions) {
        if (companion.existed) {
            if (const auto error = AtomicReplaceInPlace(companion.target, companion.backup))
                return rollback(error);
            companion.backed_up = true;
        }
        if (const auto error = publish(companion.source, companion.target))
            return rollback(error);
        companion.published = true;
    }
    // Media is published last so success requires every staged companion.
    if (const auto error = publish(from, to))
        return rollback(error);
    for (const auto& companion : companions)
        if (companion.backed_up)
            DeleteFileW(companion.backup.c_str());
    return ERROR_SUCCESS;
}

std::string DescribeFailedStagingRemoval(const std::filesystem::path& staging) {
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
