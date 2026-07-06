// install_mode_detector.cpp -- portable vs. installed detection.

#include <update/install_mode_detector.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap::update {
namespace {

// The installer (MSI) writes both values under HKLM\Software\Codexo\ExoSnap:
//   "installed"   (REG_DWORD == 1)   -- presence marker
//   "InstallPath" (REG_SZ)           -- [INSTALLFOLDER]
constexpr const wchar_t* kKeyPath = L"Software\\Codexo\\ExoSnap";

} // namespace

InstallMode DetectInstallMode() noexcept {
    static const wchar_t kValueName[] = L"installed";

    auto try_key = [&](HKEY root) -> bool {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;
        DWORD type = 0;
        DWORD data = 0;
        DWORD size = sizeof(data);
        bool found = (RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size) ==
                          ERROR_SUCCESS &&
                      type == REG_DWORD && data == 1);
        RegCloseKey(key);
        return found;
    };

    // Check HKLM first, then HKCU (per-user install).
    if (try_key(HKEY_LOCAL_MACHINE) || try_key(HKEY_CURRENT_USER))
        return InstallMode::Installed;
    return InstallMode::Portable;
}

std::optional<std::wstring> ReadInstallPath() {
    static const wchar_t kValueName[] = L"InstallPath";

    auto try_key = [&](HKEY root) -> std::optional<std::wstring> {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return std::nullopt;

        DWORD type = 0;
        DWORD size = 0;
        // First query the size (in bytes, including the terminating NUL for REG_SZ).
        if (RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ ||
            size == 0) {
            RegCloseKey(key);
            return std::nullopt;
        }

        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, kValueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(value.data()), &size) !=
            ERROR_SUCCESS) {
            RegCloseKey(key);
            return std::nullopt;
        }
        RegCloseKey(key);

        // Trim any trailing NUL characters that REG_SZ may include in the count.
        value.resize(wcslen(value.c_str()));
        return value;
    };

    if (auto v = try_key(HKEY_LOCAL_MACHINE))
        return v;
    return try_key(HKEY_CURRENT_USER);
}

} // namespace exosnap::update
