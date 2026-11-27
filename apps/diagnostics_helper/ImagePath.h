#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace exosnap::diagnostics::ipc {

// Use the object-manager root so resolving this local path never consults a drive-letter mapping.
inline std::wstring GlobalLocalVolumePath(std::wstring_view path) {
    constexpr std::wstring_view prefix = LR"(\Device\HarddiskVolume)";
    if (!path.starts_with(prefix))
        return {};
    const auto separator = path.find_first_not_of(L"0123456789", prefix.size());
    if (separator == prefix.size() || separator == std::wstring::npos || path[separator] != L'\\' ||
        separator + 1 == path.size())
        return {};
    return LR"(\\?\GLOBALROOT)" + std::wstring(path);
}

// CreateProcess consumes the Win32 device spelling. Keep GLOBALROOT so a
// different logon's drive mapping cannot select another executable.
inline std::wstring GlobalLaunchPath(std::wstring path) {
    if (!path.starts_with(LR"(\\?\GLOBALROOT\)"))
        return {};
    path[2] = L'.';
    return path;
}

inline std::wstring GlobalProcessImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    auto size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, PROCESS_NAME_NATIVE, path.data(), &size))
        return {};
    path.resize(size);
    return GlobalLocalVolumePath(path);
}

} // namespace exosnap::diagnostics::ipc
