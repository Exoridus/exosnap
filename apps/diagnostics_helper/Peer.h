#pragma once

#include "Handle.h"
#include <cstdint>
#include <string>

namespace exosnap::diagnostics::ipc {

inline std::wstring PipeName(const std::wstring& nonce) {
    if (nonce.size() != 32 || nonce.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
        return {};
    return L"\\\\.\\pipe\\ExoSnap.Diagnostics." + nonce;
}

inline uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// Windows validates the Authenticode chain. Pin the leaf certificate from the
// verified provider state, never a certificate merely embedded alongside it.
bool ApprovedPublisher(const std::wstring& image, const Handle& locked_file);
bool ApprovedClient(HANDLE process);
} // namespace exosnap::diagnostics::ipc
