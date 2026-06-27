#include "ElevationProvider.h"

#include <windows.h>

namespace exosnap::diagnostics {

bool Win32ElevationProvider::IsElevated() const {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    ::CloseHandle(token);

    if (!ok) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

} // namespace exosnap::diagnostics
