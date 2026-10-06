#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <update/distribution_context.h>
#include <update/update_types.h>

using namespace exosnap::update;

TEST(DistributionPolicy, RoutesAllOwnersInBothPhysicalModes) {
    struct Case {
        DistributionOwner owner;
        std::string_view label;
        std::string_view command;
    };
    constexpr std::array cases{
        Case{DistributionOwner::Direct, "", ""},
        Case{DistributionOwner::WinGet, "WinGet", "winget upgrade --id Codexo.ExoSnap --exact"},
        Case{DistributionOwner::Chocolatey, "Chocolatey", "choco upgrade exosnap"},
        Case{DistributionOwner::Scoop, "Scoop", "scoop update exosnap"},
        Case{DistributionOwner::UnknownManaged, "", ""},
    };
    for (const auto& item : cases) {
        for (const auto mode : {InstallMode::Installed, InstallMode::Portable}) {
            SCOPED_TRACE(DistributionOwnerToken(item.owner));
            SCOPED_TRACE(static_cast<int>(mode));
            const auto policy = ResolveUpdatePolicy({mode, item.owner});
            const bool direct = item.owner == DistributionOwner::Direct;
            const auto expected =
                direct ? (mode == InstallMode::Installed ? UpdateRoute::BuiltInMsi : UpdateRoute::BuiltInPortableSwap)
                       : UpdateRoute::ExternalPackageManager;
            EXPECT_EQ(policy.route, expected);
            EXPECT_EQ(policy.CanSelfUpdate(), direct);
            EXPECT_EQ(policy.manager_label, item.label);
            EXPECT_EQ(policy.recommended_command, item.command);
        }
    }
}

TEST(DistributionPolicy, OwnerTokensRoundTripAndInvalidTokensFailClosed) {
    struct Case {
        DistributionOwner owner;
        std::string_view token;
    };
    constexpr std::array cases{
        Case{DistributionOwner::Direct, "direct"},          Case{DistributionOwner::WinGet, "winget"},
        Case{DistributionOwner::Chocolatey, "chocolatey"},  Case{DistributionOwner::Scoop, "scoop"},
        Case{DistributionOwner::UnknownManaged, "unknown"},
    };
    for (const auto& item : cases) {
        EXPECT_EQ(DistributionOwnerToken(item.owner), item.token);
        EXPECT_EQ(DistributionOwnerFromToken(item.token), item.owner);
    }
    for (const auto token : {"", "WinGet", " direct", "direct ", "unsupported", "unknown-managed"}) {
        EXPECT_EQ(DistributionOwnerFromToken(token), DistributionOwner::UnknownManaged);
    }
    EXPECT_EQ(UpdateRouteToken(UpdateRoute::BuiltInPortableSwap), "portable-swap");
    EXPECT_EQ(UpdateRouteToken(UpdateRoute::BuiltInMsi), "msi");
    EXPECT_EQ(UpdateRouteToken(UpdateRoute::ExternalPackageManager), "external-package-manager");
}
