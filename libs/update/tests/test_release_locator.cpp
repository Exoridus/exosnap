// test_release_locator.cpp -- GitHub release/asset locator + package selection.

#include <gtest/gtest.h>
#include <update/release_locator.h>
using namespace exosnap::update;

namespace {
// v0.9.1 carries only a portable ZIP (no manifest, no signature) -> never
// qualifies. v0.9.2 carries a manifest but NO detached signature -> cannot be
// verified, so the newest-qualifying pick must skip it in favour of v0.9.0,
// which carries both the manifest and its .sig.
constexpr const char* kReleases = R"JSON([
 {"tag_name":"v0.9.2","prerelease":false,"html_url":"https://gh/r/v0.9.2","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m092.json"},
   {"name":"ExoSnap-0.9.2-windows-x64-portable.zip","browser_download_url":"https://dl/p092.zip"}]},
 {"tag_name":"v0.9.1","prerelease":false,"html_url":"https://gh/r/v0.9.1","assets":[
   {"name":"ExoSnap-0.9.1-windows-x64-portable.zip","browser_download_url":"https://dl/p091.zip"}]},
 {"tag_name":"v0.9.0","prerelease":false,"html_url":"https://gh/r/v0.9.0","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m090.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/m090.json.sig"},
   {"name":"ExoSnap-0.9.0-windows-x64-portable.zip","browser_download_url":"https://dl/p090.zip"},
   {"name":"ExoSnap-0.9.0-windows-x64.msi","browser_download_url":"https://dl/i090.msi"}]},
 {"tag_name":"v0.10.0-rc.1","prerelease":true,"html_url":"https://gh/r/rc1","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/mrc.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/mrc.json.sig"}]}
])JSON";
} // namespace

TEST(ReleaseLocator, StableSkipsReleasesMissingManifestOrSignature) {
    // Newest stable (v0.9.2) has a manifest but no .sig -> skipped; v0.9.1 has
    // neither -> skipped; v0.9.0 has both -> selected.
    auto r = LocateRelease(kReleases, UpdateChannel::Stable);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->version, (SemVer{0, 9, 0}));
    EXPECT_EQ(r->manifest_url, "https://dl/m090.json");
    EXPECT_EQ(r->signature_url, "https://dl/m090.json.sig");
    EXPECT_EQ(r->portable_url, "https://dl/p090.zip");
    EXPECT_EQ(r->installer_url, "https://dl/i090.msi");
}

TEST(ReleaseLocator, PreviewPicksPrerelease) {
    auto r = LocateRelease(kReleases, UpdateChannel::Preview);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_url, "https://dl/mrc.json");
    EXPECT_EQ(r->signature_url, "https://dl/mrc.json.sig");
}

TEST(ReleaseLocator, ManifestWithoutSignatureDoesNotQualify) {
    // A single release carrying a manifest but no detached signature must not
    // be selected -- it cannot be verified.
    constexpr const char* kNoSig = R"JSON([
     {"tag_name":"v1.0.0","prerelease":false,"html_url":"https://gh/r/v1.0.0","assets":[
       {"name":"update-manifest.json","browser_download_url":"https://dl/m100.json"}]}
    ])JSON";
    EXPECT_FALSE(LocateRelease(kNoSig, UpdateChannel::Stable).has_value());
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

// ---------------------------------------------------------------------------
// CollectReleaseNotes -- gap-aware release-note aggregation for the What's-new
// overlay. Fixture covers: an exclusive lower bound, an inclusive upper bound,
// a prerelease in range, and a draft that must always be skipped.
// ---------------------------------------------------------------------------
namespace {
constexpr const char* kNotes = R"JSON([
 {"tag_name":"v1.2.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v1.2.0","body":"## 1.2.0\n- Feature C"},
 {"tag_name":"v1.1.5-rc.1","prerelease":true,"draft":false,"html_url":"https://gh/r/v1.1.5rc1","body":"## 1.1.5-rc.1\n- Beta bits"},
 {"tag_name":"v1.1.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v1.1.0","body":"## 1.1.0\n- Feature B"},
 {"tag_name":"v1.3.0","prerelease":false,"draft":true,"html_url":"https://gh/r/v1.3.0","body":"## draft"},
 {"tag_name":"v1.0.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v1.0.0","body":"## 1.0.0\n- Feature A"}
])JSON";
}

TEST(CollectReleaseNotes, StableGapNewestFirstExclusiveLowerInclusiveUpper) {
    auto notes =
        CollectReleaseNotes(kNotes, /*above=*/SemVer{1, 0, 0}, /*up_to=*/SemVer{1, 2, 0}, UpdateChannel::Stable);
    // (1.0.0, 1.2.0] on Stable => 1.2.0, 1.1.0 (rc + draft + 1.0.0 excluded).
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].version, (SemVer{1, 2, 0}));
    EXPECT_EQ(notes[1].version, (SemVer{1, 1, 0}));
    EXPECT_EQ(notes[0].html_url, "https://gh/r/v1.2.0");
    EXPECT_NE(notes[0].body_markdown.find("Feature C"), std::string::npos);
    EXPECT_NE(notes[1].body_markdown.find("Feature B"), std::string::npos);
}

TEST(CollectReleaseNotes, PreviewIncludesPrereleaseInRange) {
    auto notes = CollectReleaseNotes(kNotes, SemVer{1, 0, 0}, SemVer{1, 2, 0}, UpdateChannel::Preview);
    // (1.0.0, 1.2.0] on Preview => 1.2.0, 1.1.5-rc.1, 1.1.0.
    ASSERT_EQ(notes.size(), 3u);
    EXPECT_EQ(notes[0].version, (SemVer{1, 2, 0}));
    EXPECT_EQ(notes[1].version, (SemVer{1, 1, 5}));
    EXPECT_EQ(notes[2].version, (SemVer{1, 1, 0}));
}

TEST(CollectReleaseNotes, ExclusiveLowerBoundOmitsInstalledVersion) {
    auto notes = CollectReleaseNotes(kNotes, SemVer{1, 1, 0}, SemVer{1, 2, 0}, UpdateChannel::Stable);
    // (1.1.0, 1.2.0] => only 1.2.0; 1.1.0 itself is excluded (exclusive lower).
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].version, (SemVer{1, 2, 0}));
}

TEST(CollectReleaseNotes, DraftIsNeverIncluded) {
    // Widen the upper bound past the draft (1.3.0): it must still be skipped.
    auto notes = CollectReleaseNotes(kNotes, SemVer{1, 0, 0}, SemVer{1, 3, 0}, UpdateChannel::Stable);
    for (const auto& n : notes)
        EXPECT_NE(n.version, (SemVer{1, 3, 0}));
}

TEST(CollectReleaseNotes, EmptyWhenRangeHasNoReleases) {
    auto notes = CollectReleaseNotes(kNotes, SemVer{1, 2, 0}, SemVer{1, 2, 0}, UpdateChannel::Stable);
    EXPECT_TRUE(notes.empty());
}

TEST(CollectReleaseNotes, MalformedJsonYieldsEmpty) {
    auto notes = CollectReleaseNotes("not json", SemVer{0, 0, 0}, SemVer{9, 9, 9}, UpdateChannel::Stable);
    EXPECT_TRUE(notes.empty());
}
