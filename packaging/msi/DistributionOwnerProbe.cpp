#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <msiquery.h>

#include <cwchar>

#if defined(EXOSNAP_OWNER_PROBE_TEST_ROOT) != defined(EXOSNAP_OWNER_PROBE_TEST_KEY)
#error Both owner probe test substitutions must be defined together.
#endif

extern "C" __declspec(dllexport) UINT __stdcall ProbeDistributionOwner(MSIHANDLE installer) {
#if defined(EXOSNAP_OWNER_PROBE_TEST_ROOT)
    const HKEY root = EXOSNAP_OWNER_PROBE_TEST_ROOT;
    const wchar_t* key_path = EXOSNAP_OWNER_PROBE_TEST_KEY;
#else
    wchar_t scope[2]{};
    DWORD scope_length = static_cast<DWORD>(sizeof(scope) / sizeof(scope[0]));
    if (MsiGetPropertyW(installer, L"ExoSnapPackageIsPerUser", scope, &scope_length) != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
    const HKEY root = scope[0] == L'1' ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    const wchar_t* key_path = L"Software\\ExoSnap";
#endif

    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, key_path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    bool owner_present = opened != ERROR_FILE_NOT_FOUND && opened != ERROR_PATH_NOT_FOUND;
    bool owner_string = false;
    if (opened == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        const LSTATUS queried = RegQueryValueExW(key, L"DistributionOwner", nullptr, &type, nullptr, &size);
        owner_present = queried != ERROR_FILE_NOT_FOUND;
        // Oversized data stays unsupported rather than being truncated into a
        // valid owner. A fixed buffer avoids allocation inside Windows Installer.
        wchar_t value[256] = {};
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

    if (MsiSetPropertyW(installer, L"EXOSNAP_DISTRIBUTION_OWNER_PRESENT", owner_present ? L"1" : L"") != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
    const UINT result = MsiSetPropertyW(installer, L"EXOSNAP_DISTRIBUTION_OWNER_STRING", owner_string ? L"1" : L"");
    return result == ERROR_SUCCESS ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

extern "C" __declspec(dllexport) UINT __stdcall ProbeLegacySetupProduct(MSIHANDLE installer) {
    wchar_t products[1024]{};
    DWORD length = static_cast<DWORD>(sizeof(products) / sizeof(products[0]));
    if (MsiGetPropertyW(installer, L"WIX_UPGRADE_DETECTED", products, &length) != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;

    // The old Burn chain hid its MSI row. Updating it behind Burn's back would
    // leave the old visible Installed Apps entry pointing at an obsolete cache.
    wchar_t prefix[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
    bool setupManaged = false;
    for (wchar_t* cursor = products; *cursor;) {
        wchar_t* separator = wcschr(cursor, L';');
        if (separator)
            *separator = L'\0';
        if (wcslen(cursor) != 38 || cursor[0] != L'{' || cursor[37] != L'}' || wcschr(cursor, L'\\'))
            return ERROR_INSTALL_FAILURE;
        wchar_t path[128]{};
        if (wcscpy_s(path, prefix) || wcscat_s(path, cursor))
            return ERROR_INSTALL_FAILURE;
        DWORD hidden = 0;
        DWORD size = sizeof(hidden);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, path, L"SystemComponent", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
                         nullptr, &hidden, &size) == ERROR_SUCCESS &&
            hidden == 1) {
            setupManaged = true;
            break;
        }
        if (!separator)
            break;
        cursor = separator + 1;
    }
    return MsiSetPropertyW(installer, L"EXOSNAP_LEGACY_SETUP_PRODUCT", setupManaged ? L"1" : L"") == ERROR_SUCCESS
               ? ERROR_SUCCESS
               : ERROR_INSTALL_FAILURE;
}
