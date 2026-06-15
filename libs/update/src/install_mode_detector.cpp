// install_mode_detector.cpp -- portable vs. installed detection.

#include <update/install_mode_detector.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap::update {

InstallMode DetectInstallMode() noexcept {
    // Check HKLM first, then HKCU (per-user install)
    static const wchar_t kKeyPath[] = L"Software\\ExoSnap";
    static const wchar_t kValueName[] = L"InstallPath";

    auto try_key = [&](HKEY root) -> bool {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;
        DWORD type = 0, size = 0;
        bool found =
            (RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ);
        RegCloseKey(key);
        return found;
    };

    if (try_key(HKEY_LOCAL_MACHINE) || try_key(HKEY_CURRENT_USER))
        return InstallMode::Installed;
    return InstallMode::Portable;
}

} // namespace exosnap::update
