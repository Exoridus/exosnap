#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace exosnap {

enum class FolderValidationResult {
    Ok,
    InvalidPath,
    NotWritable,
    CreationFailed,
};

FolderValidationResult ValidateOutputFolder(const std::filesystem::path& folder);

std::wstring FolderValidationMessage(FolderValidationResult result);

std::optional<std::filesystem::path> ResolveAvailableOutputPath(const std::filesystem::path& base_path);

} // namespace exosnap
