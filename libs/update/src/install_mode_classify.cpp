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

InstallMode ClassifyInstallMode(const std::optional<InstallStamp>& stamp,
                                const std::wstring& running_exe_dir) noexcept {
    if (!stamp.has_value() || stamp->install_dir.empty()) {
        return InstallMode::Portable;
    }
    if (running_exe_dir.empty()) {
        return InstallMode::Portable;
    }
    return NormalizeDirForCompare(stamp->install_dir) == NormalizeDirForCompare(running_exe_dir)
               ? InstallMode::Installed
               : InstallMode::Portable;
}

} // namespace exosnap::update
