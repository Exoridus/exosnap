// include/update/zip_extract.h  — no Qt, no exceptions across the boundary
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace exosnap::update {

struct ZipProgress {
    uint64_t entries_done = 0;
    uint64_t entries_total = 0;
};
using ZipProgressFn = std::function<void(const ZipProgress&)>;

// Extract every entry of `zip_path` into `dest_dir` (created if missing, must be
// empty or absent). Rejects absolute entry paths, drive letters, backslash escapes
// and any ".." component BEFORE writing anything (zip-slip guard).
// Returns std::nullopt on success, otherwise a human-readable error string.
[[nodiscard]] std::optional<std::string> ExtractZip(const std::wstring& zip_path, const std::wstring& dest_dir,
                                                    const ZipProgressFn& progress = {});

// Exposed for tests: true when `entry_name` (as stored in the zip) is safe.
[[nodiscard]] bool IsSafeZipEntryName(std::string_view entry_name);

} // namespace exosnap::update
