// install_mode_detector.cpp -- portable vs. installed detection.

#include <update/install_mode_detector.h>

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <update/update_types.h>

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

std::optional<std::wstring> ReadOwnerMarker(HKEY key) {
    DWORD type = 0;
    DWORD size = 0;
    const LSTATUS status = RegQueryValueExW(key, L"DistributionOwner", nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND)
        return std::nullopt;
    if (status != ERROR_SUCCESS || type != REG_SZ || size == 0 || size % sizeof(wchar_t) != 0)
        return std::wstring{};

    std::wstring value(size / sizeof(wchar_t), L'\0');
    const DWORD capacity = size;
    if (RegQueryValueExW(key, L"DistributionOwner", nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size) !=
            ERROR_SUCCESS ||
        type != REG_SZ || size == 0 || size > capacity || size % sizeof(wchar_t) != 0) {
        return std::wstring{};
    }
    value.resize(size / sizeof(wchar_t));
    if (value.back() != L'\0')
        return std::wstring{};
    while (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}

bool HasNonemptyString(const nlohmann::json& object, const char* property) {
    if (!object.is_object())
        return false;
    const auto value = object.find(property);
    return value != object.end() && value->is_string() && !value->get_ref<const std::string&>().empty();
}

bool HasPackageUrl(const nlohmann::json& object) {
    if (HasNonemptyString(object, "url"))
        return true;
    if (!object.is_object())
        return false;
    const auto value = object.find("url");
    if (value == object.end() || !value->is_array() || value->empty())
        return false;
    for (const auto& url : *value) {
        if (!url.is_string() || url.get_ref<const std::string&>().empty())
            return false;
    }
    return true;
}

nlohmann::json ReadScoopMetadata(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return {};
    std::ifstream file(path);
    if (!file.good())
        return {};
    return nlohmann::json::parse(file, nullptr, false);
}

bool HasLegacyScoopMetadata(const std::filesystem::path& directory) {
    const auto install = ReadScoopMetadata(directory / L"install.json");
    if (!HasNonemptyString(install, "architecture") ||
        (!HasNonemptyString(install, "bucket") && !HasNonemptyString(install, "url"))) {
        return false;
    }
    const auto& architecture = install["architecture"].get_ref<const std::string&>();
    if (architecture != "32bit" && architecture != "64bit" && architecture != "arm64")
        return false;
    const auto manifest = ReadScoopMetadata(directory / L"manifest.json");
    if (!HasNonemptyString(manifest, "version"))
        return false;
    if (HasPackageUrl(manifest))
        return true;
    const auto architectures = manifest.find("architecture");
    if (architectures == manifest.end() || !architectures->is_object())
        return false;
    const auto package = architectures->find(architecture);
    return package != architectures->end() && HasPackageUrl(*package);
}

bool HasAdjacentScoopMetadata(const std::wstring& directory) {
    if (directory.empty())
        return false;
    for (const auto* filename : {L"scoop-install.json", L"scoop-manifest.json"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(std::filesystem::path(directory) / filename, error))
            return true;
    }
    // Generic JSON filenames need the paired Scoop schemas. Presence alone can
    // otherwise claim ownership of an unrelated portable directory.
    return HasLegacyScoopMetadata(std::filesystem::path(directory));
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
        try {
            stamp.install_dir = ReadStringValue(key, L"InstallPath").value_or(std::wstring{});
            stamp.distribution_owner = ReadOwnerMarker(key);
        } catch (...) {
            RegCloseKey(key);
            throw;
        }
        RegCloseKey(key);
        return stamp;
    }
    return std::nullopt;
}

} // namespace

DistributionContext DetectDistributionContext(const std::wstring& application_directory) noexcept {
    try {
        // The first stamped record owns all installation facts. A portable copy
        // must not inherit its ownership unless the executable directory matches.
        std::optional<InstallStamp> stamp = ReadStamp(HKEY_LOCAL_MACHINE);
        if (!stamp)
            stamp = ReadStamp(HKEY_CURRENT_USER);
        const std::wstring directory = application_directory.empty() ? RunningExecutableDir() : application_directory;
        return ClassifyDistributionContext(stamp, directory, HasAdjacentScoopMetadata(directory));
    } catch (...) {
        return {InstallMode::Portable, DistributionOwner::UnknownManaged};
    }
}

InstallMode DetectInstallMode() noexcept {
    return DetectDistributionContext().install_mode;
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
