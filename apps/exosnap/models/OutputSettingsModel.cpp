#include "OutputSettingsModel.h"

#include <shlobj.h>
#include <windows.h>

namespace exosnap {

OutputSettingsModel OutputSettingsModel::Defaults() {
    OutputSettingsModel defaults;

    PWSTR videos_path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videos_path);
    if (SUCCEEDED(hr) && videos_path != nullptr && videos_path[0] != L'\0') {
        defaults.output_folder = std::filesystem::path(videos_path) / L"ExoSnap";
        CoTaskMemFree(videos_path);
    } else {
        if (videos_path != nullptr) {
            CoTaskMemFree(videos_path);
        }

        wchar_t profile[MAX_PATH] = {};
        const DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            defaults.output_folder = std::filesystem::path(profile) / L"Videos" / L"ExoSnap";
        } else {
            defaults.output_folder = std::filesystem::path(L"C:\\Users\\Public\\Videos\\ExoSnap");
        }
    }

    return defaults;
}

} // namespace exosnap
