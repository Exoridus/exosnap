#pragma once

#include <capability/config_types.h>

#include <ctime>
#include <filesystem>
#include <string>

namespace exosnap {

std::wstring BuildFilename(const std::wstring& pattern, capability::Container container, std::time_t timestamp);

std::filesystem::path BuildOutputPath(const std::filesystem::path& folder, const std::wstring& pattern,
                                      capability::Container container, std::time_t timestamp);

} // namespace exosnap
