#include "install_mode_classify.h"

#include <optional>
#include <string>
#include <update/update_types.h>

#include <algorithm>
#include <cstddef>
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

namespace {

bool HasScoopLayout(const std::wstring& directory) {
    const std::wstring path = NormalizeDirForCompare(directory);
    const std::size_t leaf_separator = path.find_last_of(L'\\');
    if (leaf_separator == std::wstring::npos || leaf_separator + 1 == path.size())
        return false;
    const std::wstring leaf = path.substr(leaf_separator + 1);
    if (leaf == L"." || leaf == L"..")
        return false;
    const std::wstring app_path = path.substr(0, leaf_separator);
    const std::wstring app_suffix = L"\\apps\\exosnap";
    if (!app_path.ends_with(app_suffix))
        return false;
    const std::wstring root = app_path.substr(0, app_path.size() - app_suffix.size());
    return root.ends_with(L"\\scoop") || leaf == L"current";
}

DistributionOwner InstalledOwner(const std::optional<std::wstring>& marker) noexcept {
    if (!marker || *marker == L"direct")
        return DistributionOwner::Direct;
    if (*marker == L"winget")
        return DistributionOwner::WinGet;
    if (*marker == L"chocolatey")
        return DistributionOwner::Chocolatey;
    return DistributionOwner::UnknownManaged;
}

} // namespace

DistributionContext ClassifyDistributionContext(const std::optional<InstallStamp>& stamp,
                                                const std::wstring& running_exe_dir, bool adjacent_scoop_metadata) {
    if (stamp && !stamp->install_dir.empty() && !running_exe_dir.empty() &&
        NormalizeDirForCompare(stamp->install_dir) == NormalizeDirForCompare(running_exe_dir)) {
        return {InstallMode::Installed, InstalledOwner(stamp->distribution_owner)};
    }
    const auto owner = adjacent_scoop_metadata || HasScoopLayout(running_exe_dir) ? DistributionOwner::Scoop
                                                                                  : DistributionOwner::Direct;
    return {InstallMode::Portable, owner};
}

InstallMode ClassifyInstallMode(const std::optional<InstallStamp>& stamp,
                                const std::wstring& running_exe_dir) noexcept {
    return ClassifyDistributionContext(stamp, running_exe_dir).install_mode;
}

} // namespace exosnap::update
