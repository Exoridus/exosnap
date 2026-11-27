#pragma once

#include "Handle.h"
#include "ImagePath.h"

#include <winternl.h>

#include <bit>
#include <limits>
#include <string_view>

namespace exosnap::diagnostics::ipc {

// Both admitted forms are absolute Object Manager names. They never start in
// the calling logon's \?? namespace. There is no create, write or rename mode.
inline Handle OpenNativeRead(std::wstring_view path, bool read_contents) {
    constexpr std::wstring_view global = LR"(\GLOBAL??\)";
    const bool global_drive = path.starts_with(global) && path.size() > global.size() + 2 &&
                              ((path[global.size()] >= L'A' && path[global.size()] <= L'Z') ||
                               (path[global.size()] >= L'a' && path[global.size()] <= L'z')) &&
                              path[global.size() + 1] == L':' && path[global.size() + 2] == L'\\';
    if ((!global_drive && LocalVolumeRootEnd(path) == std::wstring_view::npos) ||
        path.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t) ||
        path.find(L'\0') != std::wstring_view::npos) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto open = ntdll ? std::bit_cast<decltype(&NtOpenFile)>(GetProcAddress(ntdll, "NtOpenFile")) : nullptr;
    if (!open) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return {};
    }
    UNICODE_STRING name{};
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    name.Buffer = const_cast<PWSTR>(path.data());
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK io{};
    HANDLE file = nullptr;
    const auto access = (read_contents ? GENERIC_READ : FILE_READ_ATTRIBUTES) | READ_CONTROL | SYNCHRONIZE;
    const auto status = open(&file, access, &attributes, &io, FILE_SHARE_READ,
                             FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
    if (status < 0) {
        const auto error = std::bit_cast<decltype(&RtlNtStatusToDosError)>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        SetLastError(error ? error(status) : ERROR_OPEN_FAILED);
        return {};
    }
    return Handle(file);
}
} // namespace exosnap::diagnostics::ipc
