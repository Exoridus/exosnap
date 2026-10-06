#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <msiquery.h>

#if defined(EXOSNAP_OWNER_PROBE_TEST_ROOT) != defined(EXOSNAP_OWNER_PROBE_TEST_KEY)
#error Both owner probe test substitutions must be defined together.
#endif

extern "C" __declspec(dllexport) UINT __stdcall ProbeDistributionOwner(MSIHANDLE installer) {
#if defined(EXOSNAP_OWNER_PROBE_TEST_ROOT)
    const HKEY root = EXOSNAP_OWNER_PROBE_TEST_ROOT;
    const wchar_t* key_path = EXOSNAP_OWNER_PROBE_TEST_KEY;
#else
    const HKEY root = HKEY_LOCAL_MACHINE;
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
