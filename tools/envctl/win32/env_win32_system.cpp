#include "env_win32_system.h"

#include <windows.h>

namespace exosnap::envctl::win32 {
namespace {

constexpr const wchar_t* kPersonalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

std::string ReadLightDark(const wchar_t* value_name, std::string& error) {
    error.clear();
    DWORD value = 0;
    DWORD size = sizeof(value);
    // RRF_RT_REG_DWORD only; a value of another type is reported as unavailable
    // rather than coerced.
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, value_name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status != ERROR_SUCCESS) {
        error = "RegGetValueW(Personalize) failed";
        return "unavailable";
    }
    return value != 0 ? "light" : "dark";
}

} // namespace

std::string ReadAppsTheme(std::string& error) {
    return ReadLightDark(L"AppsUseLightTheme", error);
}

std::string ReadSystemTheme(std::string& error) {
    return ReadLightDark(L"SystemUsesLightTheme", error);
}

} // namespace exosnap::envctl::win32
