// install_mode_detector.cpp -- portable vs. installed detection.

#include <update/install_mode_detector.h>

#include "install_mode_classify.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap::update {
namespace {

// The installer (MSI) writes both values under HKLM\Software\ExoSnap:
//   "installed"   (REG_DWORD == 1)   -- presence marker
//   "InstallPath" (REG_SZ)           -- [INSTALLFOLDER]
// v0.10.0 wrote the same values under HKLM\Software\Codexo\ExoSnap; that key
// stays readable as a fallback while an old installation is still present,
// and is removed during the v0.10.1 upgrade.
constexpr const wchar_t* kKeyPaths[] = {L"Software\\ExoSnap", L"Software\\Codexo\\ExoSnap"};

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

// A REG_SZ value from an already-open key. Nothing when the value is absent, is
// another type, or cannot be read -- which stays distinct from a value that IS
// present and empty, because ReadInstallPath's callers rely on that difference.
std::optional<std::wstring> ReadStringValue(HKEY key, const wchar_t* value_name) {
    DWORD type = 0;
    DWORD size = 0;
    // First query the size (in bytes, including the terminating NUL for REG_SZ).
    if (RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ ||
        size == 0) {
        return std::nullopt;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, value_name, nullptr, nullptr, reinterpret_cast<LPBYTE>(value.data()), &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }

    // Trim any trailing NUL characters that REG_SZ may include in the count.
    value.resize(wcslen(value.c_str()));
    return value;
}

// One hive's stamp, read through a single open key so the marker and the path
// cannot come from different hives. Reading them independently allowed a marker
// found in HKLM to be compared against a path found in HKCU, i.e. against a
// different installation's directory, and nothing downstream could tell.
std::optional<InstallStamp> ReadStamp(HKEY root) {
    for (const wchar_t* key_path : kKeyPaths) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;

        DWORD type = 0;
        DWORD data = 0;
        DWORD size = sizeof(data);
        const bool marker_present = RegQueryValueExW(key, L"installed", nullptr, &type, reinterpret_cast<LPBYTE>(&data),
                                                     &size) == ERROR_SUCCESS &&
                                    type == REG_DWORD && data == 1;
        if (!marker_present) {
            RegCloseKey(key);
            continue;
        }

        InstallStamp stamp;
        stamp.install_dir = ReadStringValue(key, L"InstallPath").value_or(std::wstring{});
        RegCloseKey(key);
        return stamp;
    }
    return std::nullopt;
}

} // namespace

InstallMode DetectInstallMode() noexcept {
    // HKLM first, then HKCU (per-user install). The first hive carrying the
    // marker is the one whose path is compared -- never a mixture of the two.
    std::optional<InstallStamp> stamp = ReadStamp(HKEY_LOCAL_MACHINE);
    if (!stamp.has_value())
        stamp = ReadStamp(HKEY_CURRENT_USER);
    // A stamp is a fact about the MACHINE, not about this copy: a portable build
    // on a machine that also has the MSI install reads the same one.
    // ClassifyInstallMode() owns the rule; see install_mode_classify.h.
    return ClassifyInstallMode(stamp, RunningExecutableDir());
}

std::optional<std::wstring> ReadInstallPath() {
    auto try_key = [](HKEY root) -> std::optional<std::wstring> {
        for (const wchar_t* key_path : kKeyPaths) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS)
                continue;
            std::optional<std::wstring> value = ReadStringValue(key, L"InstallPath");
            RegCloseKey(key);
            if (value.has_value())
                return value;
        }
        return std::nullopt;
    };

    if (auto v = try_key(HKEY_LOCAL_MACHINE))
        return v;
    return try_key(HKEY_CURRENT_USER);
}

} // namespace exosnap::update
