#pragma once

#include <capability/config_types.h>

#include <ctime>
#include <filesystem>
#include <string>

namespace exosnap {

struct FilenameTargetContext {
    std::wstring target_name;
    std::wstring app_name;
    std::wstring process_name;
    std::wstring window_title;
};

std::wstring BuildFilename(const std::wstring& pattern, capability::Container container, std::time_t timestamp);
std::wstring BuildFilename(const std::wstring& pattern, capability::Container container, std::time_t timestamp,
                           const FilenameTargetContext& context);

std::filesystem::path BuildOutputPath(const std::filesystem::path& folder, const std::wstring& pattern,
                                      capability::Container container, std::time_t timestamp);
std::filesystem::path BuildOutputPath(const std::filesystem::path& folder, const std::wstring& pattern,
                                      capability::Container container, std::time_t timestamp,
                                      const FilenameTargetContext& context);

} // namespace exosnap
