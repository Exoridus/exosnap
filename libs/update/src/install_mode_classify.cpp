#include "install_mode_classify.h"

#include <algorithm>
#include <cwctype>

namespace exosnap::update {

std::wstring NormalizeDirForCompare(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    std::transform(path.begin(), path.end(), path.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

InstallMode ClassifyInstallMode(bool marker_present, const std::optional<std::wstring>& registry_install_dir,
                                const std::wstring& running_exe_dir) noexcept {
    if (!marker_present) {
        return InstallMode::Portable;
    }
    if (!registry_install_dir.has_value() || registry_install_dir->empty()) {
        return InstallMode::Installed;
    }
    if (running_exe_dir.empty()) {
        // The caller could not resolve its own location. The marker is the only
        // fact left, and it says installed.
        return InstallMode::Installed;
    }
    return NormalizeDirForCompare(*registry_install_dir) == NormalizeDirForCompare(running_exe_dir)
               ? InstallMode::Installed
               : InstallMode::Portable;
}

} // namespace exosnap::update
