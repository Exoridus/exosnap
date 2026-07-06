// test_release_locator.cpp -- GitHub release/asset locator + package selection.

#include <gtest/gtest.h>
#include <update/release_locator.h>
using namespace exosnap::update;

namespace {
constexpr const char* kReleases = R"JSON([
 {"tag_name":"v0.9.1","prerelease":false,"html_url":"https://gh/r/v0.9.1","assets":[
   {"name":"ExoSnap-0.9.1-windows-x64-portable.zip","browser_download_url":"https://dl/p091.zip"}]},
 {"tag_name":"v0.9.0","prerelease":false,"html_url":"https://gh/r/v0.9.0","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m090.json"},
   {"name":"ExoSnap-0.9.0-windows-x64-portable.zip","browser_download_url":"https://dl/p090.zip"},
   {"name":"ExoSnap-0.9.0-windows-x64.msi","browser_download_url":"https://dl/i090.msi"}]},
 {"tag_name":"v0.10.0-rc.1","prerelease":true,"html_url":"https://gh/r/rc1","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/mrc.json"}]}
])JSON";
}

TEST(ReleaseLocator, StableSkipsReleasesWithoutManifestAsset) {
    auto r = LocateRelease(kReleases, UpdateChannel::Stable);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->version, (SemVer{0, 9, 0}));
    EXPECT_EQ(r->manifest_url, "https://dl/m090.json");
    EXPECT_EQ(r->portable_url, "https://dl/p090.zip");
    EXPECT_EQ(r->installer_url, "https://dl/i090.msi");
}

TEST(ReleaseLocator, PreviewPicksPrerelease) {
    auto r = LocateRelease(kReleases, UpdateChannel::Preview);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_url, "https://dl/mrc.json");
}

TEST(ReleaseLocator, MalformedJsonSetsParseError) {
    std::string parse_error;
    auto r = LocateRelease("not json", UpdateChannel::Stable, &parse_error);
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(parse_error.empty());
}

TEST(ReleaseLocator, ValidEmptyArrayLeavesParseErrorEmpty) {
    std::string parse_error;
    auto r = LocateRelease("[]", UpdateChannel::Stable, &parse_error);
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(parse_error.empty());
}

TEST(ReleaseLocator, SelectPackageByInstallMode) {
    UpdateManifest m;
    m.packages = {{PackageKind::Installer, "https://dl/i.msi", "aa"},
                  {PackageKind::Portable, "https://dl/p.zip", "bb"}};
    EXPECT_EQ(SelectPackage(m, InstallMode::Installed)->url, "https://dl/i.msi");
    EXPECT_EQ(SelectPackage(m, InstallMode::Portable)->url, "https://dl/p.zip");
    m.packages.pop_back();
    EXPECT_EQ(SelectPackage(m, InstallMode::Portable), nullptr);
}
