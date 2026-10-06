#pragma once

#include <string_view>
#include <update/update_types.h>

namespace exosnap::update {

enum class UpdateRoute : uint8_t {
    BuiltInPortableSwap = 0,
    BuiltInMsi = 1,
    ExternalPackageManager = 2,
};

struct UpdatePolicy {
    UpdateRoute route = UpdateRoute::BuiltInPortableSwap;
    std::string_view manager_label;
    std::string_view recommended_command;

    [[nodiscard]] bool CanSelfUpdate() const noexcept;
};

[[nodiscard]] UpdatePolicy ResolveUpdatePolicy(DistributionContext distribution) noexcept;
[[nodiscard]] std::string_view DistributionOwnerToken(DistributionOwner owner) noexcept;
// Invalid explicit tokens, including an empty token, remain externally managed.
[[nodiscard]] DistributionOwner DistributionOwnerFromToken(std::string_view token) noexcept;
[[nodiscard]] std::string_view UpdateRouteToken(UpdateRoute route) noexcept;

} // namespace exosnap::update
