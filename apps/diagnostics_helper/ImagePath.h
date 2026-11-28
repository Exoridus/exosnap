#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace exosnap::diagnostics::ipc {

// Validate a local kernel volume path before spelling it for a Win32 consumer.
inline size_t LocalVolumeRootEnd(std::wstring_view path) {
    constexpr std::wstring_view prefix = LR"(\Device\HarddiskVolume)";
    if (!path.starts_with(prefix))
        return std::wstring_view::npos;
    const auto separator = path.find_first_not_of(L"0123456789", prefix.size());
    if (separator == prefix.size() || separator == std::wstring_view::npos || path[separator] != L'\\' ||
        separator + 1 == path.size())
        return std::wstring_view::npos;
    return separator;
}

inline std::wstring GlobalLocalVolumePath(std::wstring_view path) {
    if (LocalVolumeRootEnd(path) == std::wstring_view::npos)
        return {};
    return LR"(\\?\GLOBALROOT)" + std::wstring(path);
}

// CreateProcess consumes the Win32 device spelling. GLOBALROOT resolution
// requires a trusted consumer device map; file reads use absolute NT names.
inline std::wstring GlobalLaunchPath(std::wstring path) {
    if (!path.starts_with(LR"(\\?\GLOBALROOT\)"))
        return {};
    path[2] = L'.';
    return path;
}

inline std::wstring NativeProcessImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    auto size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, PROCESS_NAME_NATIVE, path.data(), &size))
        return {};
    path.resize(size);
    return LocalVolumeRootEnd(path) == std::wstring_view::npos ? std::wstring{} : path;
}

inline std::wstring GlobalProcessImagePath(HANDLE process) {
    return GlobalLocalVolumePath(NativeProcessImagePath(process));
}

} // namespace exosnap::diagnostics::ipc
