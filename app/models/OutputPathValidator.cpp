#include "OutputPathValidator.h"
#include <capability/translatable.h>

#include <exosnap/engine/recorder_session.h>

#include <objbase.h>
#include <windows.h>

#include <format>

namespace exosnap {

bool ProbeOutputDirectoryWritable(const std::filesystem::path& folder) {
    // Exclusive creation and handle-owned cleanup leave existing files untouched even if names collide.
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        GUID id{};
        if (FAILED(CoCreateGuid(&id)))
            return false;
        // Keep the leaf within the existing MAX_PATH budget for recording folders.
        const auto path = folder / std::format(L".es{:08x}{:04x}.tmp", id.Data1, id.Data2);
        file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS)
            return false;
    }
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const char payload[] = "ok";
    DWORD written = 0;
    // A delayed write failure must be observed before reporting the directory writable.
    const bool flushed = WriteFile(file, payload, sizeof(payload), &written, nullptr) && written == sizeof(payload) &&
                         FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    return flushed && closed;
}

FolderValidationResult ValidateOutputFolder(const std::filesystem::path& folder) {
    if (folder.empty()) {
        return FolderValidationResult::InvalidPath;
    }

    std::error_code ec;
    const bool exists = std::filesystem::exists(folder, ec);
    if (ec) {
        return FolderValidationResult::InvalidPath;
    }

    if (exists) {
        const bool is_dir = std::filesystem::is_directory(folder, ec);
        if (ec || !is_dir) {
            return FolderValidationResult::InvalidPath;
        }
    } else {
        std::filesystem::create_directories(folder, ec);
        if (ec) {
            return FolderValidationResult::CreationFailed;
        }
    }

    return ProbeOutputDirectoryWritable(folder) ? FolderValidationResult::Ok : FolderValidationResult::NotWritable;
}

// No `default:`: the enum is ours and closed, so a new validation outcome must
// fail the build (C4062 under /W4 /WX) instead of reaching the user as the
// generic sentence. The trailing return is only for the compiler's benefit.
std::wstring FolderValidationMessage(FolderValidationResult result) {
    switch (result) {
    case FolderValidationResult::Ok:
        return L"OK";
    case FolderValidationResult::InvalidPath:
        return EXOSNAP_TRANSLATABLE("OutputSettings", L"Output folder path is invalid.");
    case FolderValidationResult::NotWritable:
        return EXOSNAP_TRANSLATABLE("OutputSettings", L"Output folder is not writable.");
    case FolderValidationResult::CreationFailed:
        return EXOSNAP_TRANSLATABLE("OutputSettings", L"Failed to create output folder.");
    }
    return EXOSNAP_TRANSLATABLE("OutputSettings", L"Output folder validation failed.");
}

std::optional<std::filesystem::path> ResolveAvailableOutputPath(const std::filesystem::path& base_path) {
    const auto is_available = [](const std::filesystem::path& candidate, std::error_code& ec) {
        if (std::filesystem::exists(candidate, ec) || ec) {
            return false;
        }
        return !std::filesystem::exists(exosnap::engine::DeriveValuablePartialPath(candidate), ec) && !ec;
    };

    std::error_code ec;
    if (is_available(base_path, ec)) {
        return base_path;
    }
    if (ec) {
        return std::nullopt;
    }

    const auto stem = base_path.stem().wstring();
    const auto ext = base_path.extension().wstring();
    const auto parent = base_path.parent_path();

    for (int suffix = 1; suffix < 1000; ++suffix) {
        const auto candidate = parent / (std::wstring(stem) + L" (" + std::to_wstring(suffix) + L")" + ext);
        if (is_available(candidate, ec)) {
            return candidate;
        }
        if (ec) {
            break;
        }
    }

    return std::nullopt;
}

} // namespace exosnap
