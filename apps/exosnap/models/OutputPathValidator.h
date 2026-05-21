#pragma once

#include <filesystem>
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

} // namespace exosnap
