#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <msiquery.h>

#include <cwchar>
#include <initializer_list>

#if defined(EXOSNAP_OWNER_PROBE_TEST_ROOT) != defined(EXOSNAP_OWNER_PROBE_TEST_KEY)
#error Both owner probe test substitutions must be defined together.
#endif

namespace {
template <size_t N> bool ReadProperty(MSIHANDLE installer, const wchar_t* name, wchar_t (&value)[N]) {
    DWORD size = static_cast<DWORD>(N);
    return MsiGetPropertyW(installer, name, value, &size) == ERROR_SUCCESS;
}

bool ReadRegistered(HKEY hive, bool& present) {
    for (const auto* path : {L"Software\\ExoSnap", L"Software\\Codexo\\ExoSnap"}) {
        DWORD marker = 0;
        DWORD bytes = sizeof(marker);
        const auto status = RegGetValueW(hive, path, L"installed", RRF_RT_REG_DWORD, nullptr, &marker, &bytes);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
            continue;
        if (status != ERROR_SUCCESS || marker != 1)
            return false;
        present = true;
        break;
    }
    return true;
}
// Old Setup packages hid the chained MSI before the explicit SetupManaged
// marker existed. A raw major upgrade must not strand their cached bundle.
UINT CheckRelatedSetup(MSIHANDLE installer, HKEY hive) {
    wchar_t codes[2048]{};
    if (!ReadProperty(installer, L"WIX_UPGRADE_DETECTED", codes))
        return ERROR_INSTALL_FAILURE;
    if (!*codes)
        return ERROR_SUCCESS;
    HKEY uninstall = nullptr;
    const auto opened = RegOpenKeyExW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                                      KEY_READ | KEY_WOW64_64KEY, &uninstall);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND)
        return ERROR_SUCCESS;
    if (opened != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
    UINT result = ERROR_SUCCESS;
    for (auto* code = codes; code && *code;) {
        auto* next = wcschr(code, L';');
        if (next)
            *next++ = L'\0';
        DWORD hidden = 0;
        DWORD bytes = sizeof(hidden);
        if (RegGetValueW(uninstall, code, L"SystemComponent", RRF_RT_REG_DWORD, nullptr, &hidden, &bytes) ==
                ERROR_SUCCESS &&
            hidden == 1) {
            result = MsiSetPropertyW(installer, L"ExoSnapHiddenRelatedMsi", L"1");
            break;
        }
        code = next;
    }
    RegCloseKey(uninstall);
    return result;
}
} // namespace

extern "C" __declspec(dllexport) UINT __stdcall SelectInstallScope(MSIHANDLE installer) {
    wchar_t requested[16]{}, owner[32]{}, all_users[8]{}, installed[32]{};
    if (!ReadProperty(installer, L"EXOSNAP_REQUESTED_SCOPE", requested) ||
        !ReadProperty(installer, L"EXOSNAP_DISTRIBUTION_OWNER", owner) ||
        !ReadProperty(installer, L"ALLUSERS", all_users) || !ReadProperty(installer, L"Installed", installed))
        return ERROR_INSTALL_FAILURE;

    // Maintenance is bound to an existing Windows Installer product context.
    if (*installed && !*requested)
        return ERROR_SUCCESS;
    bool machine = false, user = false;
    if (!ReadRegistered(HKEY_LOCAL_MACHINE, machine) || !ReadRegistered(HKEY_CURRENT_USER, user))
        return ERROR_INSTALL_FAILURE;
    const bool manager = wcscmp(owner, L"winget") == 0 || wcscmp(owner, L"chocolatey") == 0;
    const bool choose_machine =
        wcscmp(requested, L"machine") == 0 || (!*requested && (machine || *all_users || manager));
    if ((*requested && wcscmp(requested, L"user") != 0 && wcscmp(requested, L"machine") != 0) ||
        (choose_machine ? user : machine)) {
        return MsiSetPropertyW(installer, L"EXOSNAP_SCOPE_CONFLICT", L"1");
    }
    return *installed ? ERROR_SUCCESS : MsiSetPropertyW(installer, L"ALLUSERS", choose_machine ? L"1" : L"");
}

extern "C" __declspec(dllexport) UINT __stdcall ProbeDistributionOwner(MSIHANDLE installer) {
#if defined(EXOSNAP_OWNER_PROBE_TEST_ROOT)
    const HKEY root = EXOSNAP_OWNER_PROBE_TEST_ROOT;
    const wchar_t* key_path = EXOSNAP_OWNER_PROBE_TEST_KEY;
#else
    wchar_t all_users[8]{};
    if (!ReadProperty(installer, L"ALLUSERS", all_users))
        return ERROR_INSTALL_FAILURE;
    const HKEY root = *all_users ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const wchar_t* key_path = L"Software\\ExoSnap";
#endif

    if (CheckRelatedSetup(installer, root) != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, key_path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    bool owner_present = opened != ERROR_FILE_NOT_FOUND && opened != ERROR_PATH_NOT_FOUND;
    bool owner_string = false;
    wchar_t value[256] = {};
    if (opened == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        const LSTATUS queried = RegQueryValueExW(key, L"DistributionOwner", nullptr, &type, nullptr, &size);
        owner_present = queried != ERROR_FILE_NOT_FOUND;
        // Oversized data stays unsupported rather than being truncated into a
        // valid owner. A fixed buffer avoids allocation inside Windows Installer.
        if (queried == ERROR_SUCCESS && type == REG_SZ && size != 0 && size <= sizeof(value) &&
            size % sizeof(wchar_t) == 0) {
            DWORD read_type = 0;
            DWORD read_size = size;
            const LSTATUS read = RegQueryValueExW(key, L"DistributionOwner", nullptr, &read_type,
                                                  reinterpret_cast<LPBYTE>(value), &read_size);
            if (read == ERROR_SUCCESS && read_type == REG_SZ && read_size != 0 && read_size <= size &&
                read_size % sizeof(wchar_t) == 0) {
                const DWORD characters = read_size / static_cast<DWORD>(sizeof(wchar_t));
                owner_string = value[characters - 1] == L'\0';
                bool terminated = false;
                for (DWORD index = 0; index < characters && owner_string; ++index) {
                    if (value[index] == L'\0')
                        terminated = true;
                    else if (terminated)
                        owner_string = false;
                }
            }
        }
        RegCloseKey(key);
    }

    // AppSearch reads the machine marker. For a user install only the selected
    // user's record can contribute the remembered value.
#if !defined(EXOSNAP_OWNER_PROBE_TEST_ROOT)
    if (root == HKEY_CURRENT_USER && MsiSetPropertyW(installer, L"EXOSNAP_DISTRIBUTION_OWNER_REMEMBERED",
                                                     owner_string ? value : L"") != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
#endif
    if (MsiSetPropertyW(installer, L"EXOSNAP_DISTRIBUTION_OWNER_PRESENT", owner_present ? L"1" : L"") != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
    const UINT result = MsiSetPropertyW(installer, L"EXOSNAP_DISTRIBUTION_OWNER_STRING", owner_string ? L"1" : L"");
    return result == ERROR_SUCCESS ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}
