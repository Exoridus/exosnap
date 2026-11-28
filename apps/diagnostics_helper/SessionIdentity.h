#pragma once

#include <windows.h>

namespace exosnap::diagnostics::ipc {

// A shared descriptive name limits the product to one privileged session;
// the independent per-activation GUID proves which generation may be reaped.
inline constexpr wchar_t kPrivilegedSessionName[] = L"ExoSnap Privileged Presentation";
inline GUID DpcIdentity(GUID id) {
    id.Data1 ^= 0x80000000u;
    return id;
}
} // namespace exosnap::diagnostics::ipc
