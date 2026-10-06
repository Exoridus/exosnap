#include <update/distribution_context.h>
#include <update/update_types.h>

#include <string_view>

namespace exosnap::update {

bool UpdatePolicy::CanSelfUpdate() const noexcept {
    return route != UpdateRoute::ExternalPackageManager;
}

UpdatePolicy ResolveUpdatePolicy(DistributionContext distribution) noexcept {
    switch (distribution.owner) {
    case DistributionOwner::Direct:
        return {distribution.install_mode == InstallMode::Installed ? UpdateRoute::BuiltInMsi
                                                                    : UpdateRoute::BuiltInPortableSwap,
                {},
                {}};
    case DistributionOwner::WinGet:
        return {UpdateRoute::ExternalPackageManager, "WinGet", "winget upgrade --id Codexo.ExoSnap --exact"};
    case DistributionOwner::Chocolatey:
        return {UpdateRoute::ExternalPackageManager, "Chocolatey", "choco upgrade exosnap"};
    case DistributionOwner::Scoop:
        return {UpdateRoute::ExternalPackageManager, "Scoop", "scoop update exosnap"};
    case DistributionOwner::UnknownManaged:
        return {UpdateRoute::ExternalPackageManager, {}, {}};
    }
    return {UpdateRoute::ExternalPackageManager, {}, {}};
}

std::string_view DistributionOwnerToken(DistributionOwner owner) noexcept {
    switch (owner) {
    case DistributionOwner::Direct:
        return "direct";
    case DistributionOwner::WinGet:
        return "winget";
    case DistributionOwner::Chocolatey:
        return "chocolatey";
    case DistributionOwner::Scoop:
        return "scoop";
    case DistributionOwner::UnknownManaged:
        return "unknown";
    }
    return "unknown";
}

DistributionOwner DistributionOwnerFromToken(std::string_view token) noexcept {
    if (token == "direct")
        return DistributionOwner::Direct;
    if (token == "winget")
        return DistributionOwner::WinGet;
    if (token == "chocolatey")
        return DistributionOwner::Chocolatey;
    if (token == "scoop")
        return DistributionOwner::Scoop;
    return DistributionOwner::UnknownManaged;
}

std::string_view UpdateRouteToken(UpdateRoute route) noexcept {
    switch (route) {
    case UpdateRoute::BuiltInPortableSwap:
        return "portable-swap";
    case UpdateRoute::BuiltInMsi:
        return "msi";
    case UpdateRoute::ExternalPackageManager:
        return "external-package-manager";
    }
    return "external-package-manager";
}

} // namespace exosnap::update
