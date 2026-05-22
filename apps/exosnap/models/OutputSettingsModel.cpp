#include "OutputSettingsModel.h"

#include <windows.h>

namespace exosnap {

OutputSettingsModel OutputSettingsModel::Defaults() {
    OutputSettingsModel defaults;

    wchar_t profile[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        defaults.output_folder = std::filesystem::path(profile) / L"Videos" / L"ExoSnap";
    } else {
        defaults.output_folder = std::filesystem::path(L"C:\\Users\\Public\\Videos\\ExoSnap");
    }

    return defaults;
}

} // namespace exosnap
