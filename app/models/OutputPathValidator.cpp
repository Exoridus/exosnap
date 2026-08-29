#include "OutputPathValidator.h"

#include <exosnap/engine/recorder_session.h>

#include <fstream>

namespace exosnap {

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

    const std::filesystem::path write_probe = folder / L".exosnap_write_test";
    {
        std::ofstream probe(write_probe, std::ios::binary | std::ios::trunc);
        if (!probe.is_open()) {
            return FolderValidationResult::NotWritable;
        }
        probe << "ok";
        // A stream write only reaches the buffer. Checking good() here says
        // nothing about whether the bytes ever reached the volume: a full disk, a
        // quota boundary or an offline network share fails at the flush, and
        // ~ofstream swallows that failure without a trace. So flush and close
        // explicitly and judge the stream AFTER the close — an unwritable folder
        // that accepts open() must still be reported as NotWritable.
        probe.flush();
        probe.close();
        if (!probe.good()) {
            std::error_code cleanup_ec;
            std::filesystem::remove(write_probe, cleanup_ec);
            return FolderValidationResult::NotWritable;
        }
    }

    std::filesystem::remove(write_probe, ec);
    if (ec) {
        return FolderValidationResult::NotWritable;
    }

    return FolderValidationResult::Ok;
}

// No `default:`: the enum is ours and closed, so a new validation outcome must
// fail the build (C4062 under /W4 /WX) instead of reaching the user as the
// generic sentence. The trailing return is only for the compiler's benefit.
std::wstring FolderValidationMessage(FolderValidationResult result) {
    switch (result) {
    case FolderValidationResult::Ok:
        return L"OK";
    case FolderValidationResult::InvalidPath:
        return L"Output folder path is invalid.";
    case FolderValidationResult::NotWritable:
        return L"Output folder is not writable.";
    case FolderValidationResult::CreationFailed:
        return L"Failed to create output folder.";
    }
    return L"Output folder validation failed.";
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
