#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace exosnap::update {

struct DownloadProgress {
    uint64_t bytes_received = 0;
    uint64_t bytes_total = 0; // 0 when Content-Length is unknown
};
using DownloadProgressFn = std::function<void(const DownloadProgress&)>;

// HTTPS GET `url` streamed into `dest_path` (overwritten; parent dir must exist).
// 64 KiB chunks; `cancel` is checked between chunks — when set, the partial file is
// deleted and "canceled" is returned. Follows redirects (WinHTTP default policy);
// https-only (rejects http:// URLs). Returns std::nullopt on success.
[[nodiscard]] std::optional<std::string> DownloadToFile(const std::string& url, const std::wstring& dest_path,
                                                        const DownloadProgressFn& progress,
                                                        const std::atomic<bool>& cancel);

} // namespace exosnap::update
