// test_update_checker.cpp -- BuildCheckResult: the network-independent part of
// CheckForUpdate's result assembly (offer decision, gap notes, all-channel notes).

#include <gtest/gtest.h>
#include <update/release_locator.h>
#include <update/update_checker.h>

using namespace exosnap::update;

namespace {

// v0.9.0 and v0.10.0-rc.1 both carry a manifest + signature (qualify for LocateRelease);
// v0.8.0 is stable and older than both. All three carry a body for note assembly.
constexpr const char* kReleases = R"JSON([
 {"tag_name":"v0.10.0-rc.1","prerelease":true,"draft":false,"html_url":"https://gh/r/rc1","body":"## 0.10.0-rc.1\n- Preview bits","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/mrc.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/mrc.json.sig"}]},
 {"tag_name":"v0.9.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v0.9.0","body":"## 0.9.0\n- Stable bits","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m090.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/m090.json.sig"}]},
 {"tag_name":"v0.8.0","prerelease":false,"draft":false,"html_url":"https://gh/r/v0.8.0","body":"## 0.8.0\n- Old bits","assets":[
   {"name":"update-manifest.json","browser_download_url":"https://dl/m080.json"},
   {"name":"update-manifest.json.sig","browser_download_url":"https://dl/m080.json.sig"}]}
])JSON";

CheckParams StableParamsAt(const char* current) {
    CheckParams p;
    p.current_version = *ParseSemVer(current);
    p.current_version_raw = current;
    p.channel = UpdateChannel::Stable;
    return p;
}

} // namespace

TEST(BuildCheckResult, AllChannelNotesPopulatedWhenAlreadyUpToDate) {
    // Current == the newest stable release: no update offered, but the reference
    // list must still be populated.
    auto params = StableParamsAt("0.9.0");
    auto release = LocateRelease(kReleases, UpdateChannel::Stable);
    ASSERT_TRUE(release.has_value());

    auto result = BuildCheckResult(kReleases, release, params);

    EXPECT_FALSE(result.update_available);
    EXPECT_TRUE(result.gap_notes.empty());
    ASSERT_EQ(result.all_channel_notes.size(), 2u); // 0.9.0, 0.8.0 (Stable excludes the rc)
    EXPECT_EQ(result.all_channel_notes[0].version, (SemVer{0, 9, 0}));
    EXPECT_EQ(result.all_channel_notes[1].version, (SemVer{0, 8, 0}));
}

TEST(BuildCheckResult, AllChannelNotesPopulatedWhenUpdateAvailable) {
    auto params = StableParamsAt("0.8.0");
    auto release = LocateRelease(kReleases, UpdateChannel::Stable);
    ASSERT_TRUE(release.has_value());

    auto result = BuildCheckResult(kReleases, release, params);

    EXPECT_TRUE(result.update_available);
    ASSERT_EQ(result.gap_notes.size(), 1u); // (0.8.0, 0.9.0] => 0.9.0 only
    EXPECT_EQ(result.gap_notes[0].version, (SemVer{0, 9, 0}));
    ASSERT_EQ(result.all_channel_notes.size(), 2u); // unaffected by the gap
    EXPECT_EQ(result.all_channel_notes[0].version, (SemVer{0, 9, 0}));
    EXPECT_EQ(result.all_channel_notes[1].version, (SemVer{0, 8, 0}));
}

TEST(BuildCheckResult, AllChannelNotesRespectsChannelWhenNoReleaseLocates) {
    // Preview: newest qualifying release is the rc. current_version already equals it,
    // so no update is offered, but all_channel_notes must still include the rc.
    CheckParams params;
    params.current_version = *ParseSemVer("0.10.0-rc.1");
    params.current_version_raw = "0.10.0-rc.1";
    params.channel = UpdateChannel::Preview;
    auto release = LocateRelease(kReleases, UpdateChannel::Preview);
    ASSERT_TRUE(release.has_value());

    auto result = BuildCheckResult(kReleases, release, params);

    EXPECT_FALSE(result.update_available);
    ASSERT_EQ(result.all_channel_notes.size(), 3u);
    EXPECT_EQ(result.all_channel_notes[0].version, (SemVer{0, 10, 0, true, 0}));
}

TEST(BuildCheckResult, NoReleaseLocatedStillPopulatesAllChannelNotes) {
    // No release at all locates (e.g. everything filtered out elsewhere) -- release is
    // nullopt, but the reference list is independent of LocateRelease's pick.
    auto params = StableParamsAt("0.9.0");
    auto result = BuildCheckResult(kReleases, std::nullopt, params);

    EXPECT_FALSE(result.update_available);
    EXPECT_TRUE(result.gap_notes.empty());
    ASSERT_EQ(result.all_channel_notes.size(), 2u);
}
