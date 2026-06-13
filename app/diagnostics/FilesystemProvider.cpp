#include "FilesystemProvider.h"

#include <windows.h>

namespace exosnap::diagnostics {

std::string Win32FilesystemProvider::FilesystemNameForPath(const std::filesystem::path& path) const {
    if (path.empty())
        return {};

    // Walk up to the nearest ancestor that actually exists so we can still
    // query the volume even when the output folder has not been created yet.
    std::filesystem::path query_path = path;
    std::error_code ec;
    while (!query_path.empty() && !std::filesystem::exists(query_path, ec)) {
        const std::filesystem::path parent = query_path.parent_path();
        if (parent == query_path)
            break; // reached root without finding anything
        query_path = parent;
    }

    // GetVolumeInformationW requires a root path (e.g. "C:\").
    // Derive the volume root from the resolved path.
    std::wstring volume_root;
    {
        std::wstring wide_path = query_path.wstring();
        // Ensure the path ends with a backslash so GetVolumeInformationW
        // accepts it as a root or directory path.
        if (!wide_path.empty() && wide_path.back() != L'\\' && wide_path.back() != L'/') {
            wide_path += L'\\';
        }

        // Extract just the volume root component.
        wchar_t volume_path_buf[MAX_PATH + 1] = {};
        if (!::GetVolumePathNameW(wide_path.c_str(), volume_path_buf, MAX_PATH + 1)) {
            return {};
        }
        volume_root = volume_path_buf;
    }

    wchar_t fs_name[MAX_PATH + 1] = {};
    if (!::GetVolumeInformationW(volume_root.c_str(),
                                 /*lpVolumeNameBuffer=*/nullptr,
                                 /*nVolumeNameSize=*/0,
                                 /*lpVolumeSerialNumber=*/nullptr,
                                 /*lpMaximumComponentLength=*/nullptr,
                                 /*lpFileSystemFlags=*/nullptr, fs_name, MAX_PATH + 1)) {
        return {};
    }

    // Convert to narrow string (filesystem names are ASCII).
    std::string result;
    result.reserve(wcslen(fs_name));
    for (const wchar_t* p = fs_name; *p != L'\0'; ++p) {
        result += static_cast<char>(*p);
    }
    return result;
}

} // namespace exosnap::diagnostics
