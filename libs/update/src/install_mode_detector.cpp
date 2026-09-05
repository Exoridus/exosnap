// install_mode_detector.cpp -- portable vs. installed detection.

#include <update/install_mode_detector.h>

#include "install_mode_classify.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap::update {
namespace {

// The installer (MSI) writes both values under HKLM\Software\Codexo\ExoSnap:
//   "installed"   (REG_DWORD == 1)   -- presence marker
//   "InstallPath" (REG_SZ)           -- [INSTALLFOLDER]
constexpr const wchar_t* kKeyPath = L"Software\\Codexo\\ExoSnap";

// Directory of the running executable, empty when it cannot be resolved.
std::wstring RunningExecutableDir() {
    std::wstring buffer;
    buffer.resize(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
            return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        // Truncated: the path is longer than MAX_PATH, which a portable copy
        // extracted into a deep folder really can be.
        if (buffer.size() >= 32768)
            return {};
        buffer.resize(buffer.size() * 2);
    }
    const size_t slash = buffer.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    buffer.resize(slash);
    return buffer;
}

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
    const bool marker_present = try_key(HKEY_LOCAL_MACHINE) || try_key(HKEY_CURRENT_USER);
    // The marker alone is a fact about the MACHINE, not about this copy: a
    // portable build on a machine that also has the MSI install would inherit it.
    // ClassifyInstallMode() owns the rule; see install_mode_classify.h.
    return ClassifyInstallMode(marker_present, ReadInstallPath(), RunningExecutableDir());
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
