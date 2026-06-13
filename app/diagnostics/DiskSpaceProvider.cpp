#include "DiskSpaceProvider.h"

#include <windows.h>

namespace exosnap::diagnostics {

uint64_t Win32DiskSpaceProvider::FreeBytesForPath(const std::filesystem::path& path) const {
    if (path.empty())
        return 0;

    // Query the volume that hosts `path`.  We use the directory component; if
    // the path does not exist yet (the output folder may not have been created
    // yet) we walk up to the nearest ancestor that does exist so we still get
    // the correct volume.
    std::filesystem::path query_path = path;
    std::error_code ec;
    while (!query_path.empty() && !std::filesystem::exists(query_path, ec)) {
        const std::filesystem::path parent = query_path.parent_path();
        if (parent == query_path)
            break; // reached root and it doesn't exist either
        query_path = parent;
    }

    ULARGE_INTEGER free_bytes_available{};
    ULARGE_INTEGER total_bytes{};
    ULARGE_INTEGER total_free_bytes{};

    const std::wstring wide_path = query_path.wstring();
    if (!::GetDiskFreeSpaceExW(wide_path.c_str(), &free_bytes_available, &total_bytes, &total_free_bytes)) {
        return 0;
    }

    // free_bytes_available is the bytes available to the calling user (respects
    // per-user quotas); prefer that over total_free_bytes.
    return static_cast<uint64_t>(free_bytes_available.QuadPart);
}

} // namespace exosnap::diagnostics
