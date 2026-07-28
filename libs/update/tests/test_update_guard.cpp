// test_update_guard.cpp -- UpdateBlockReason / recording guard tests.
//
// We test the guard contract directly via CheckParams::recording_guard without
// actually making a network request (EXOSNAP_OFFICIAL_BUILD is off in tests).

#include <gtest/gtest.h>
#include <update/update_checker.h>
#include <update/update_types.h>

using namespace exosnap::update;

TEST(UpdateGuard, OfficialBuildGateOff) {
    // With EXOSNAP_OFFICIAL_BUILD absent (test build), IsUpdateCheckEnabled
    // must return false.
    EXPECT_FALSE(IsUpdateCheckEnabled());
}

TEST(UpdateGuard, CheckReturnsBecauseOfficialBuildOff) {
    CheckParams p;
    p.current_version = SemVer{0, 3, 0};
    p.channel = UpdateChannel::Stable;
    auto result = CheckForUpdate(p);
    // Must return an error result, NOT make a network request
    EXPECT_TRUE(result.check_failed);
    EXPECT_TRUE(result.error_message.has_value());
}

TEST(UpdateGuard, RecordingActiveBlocksCheck) {
    // Even if official build gate were on, active recording must block.
    CheckParams p;
    p.current_version = SemVer{0, 3, 0};
    p.recording_guard = []() { return UpdateBlockReason::ActiveRecording; };
    auto result = CheckForUpdate(p);
    EXPECT_TRUE(result.check_failed);
    ASSERT_TRUE(result.error_message.has_value());
    EXPECT_NE(result.error_message->find("recording"), std::string::npos);
}

TEST(UpdateGuard, FinalizingBlocksCheck) {
    CheckParams p;
    p.current_version = SemVer{0, 3, 0};
    p.recording_guard = []() { return UpdateBlockReason::Finalizing; };
    auto result = CheckForUpdate(p);
    EXPECT_TRUE(result.check_failed);
    ASSERT_TRUE(result.error_message.has_value());
    EXPECT_NE(result.error_message->find("finaliz"), std::string::npos);
}

TEST(UpdateGuard, NotBlockedGuardPassesThrough) {
    // NotBlocked guard should not cause a block (but still fails due to
    // EXOSNAP_OFFICIAL_BUILD being off in test builds)
    CheckParams p;
    p.current_version = SemVer{0, 3, 0};
    p.recording_guard = []() { return UpdateBlockReason::NotBlocked; };
    auto result = CheckForUpdate(p);
    // The official-build gate fires before the network is touched
    EXPECT_TRUE(result.check_failed);
}

// ---------------------------------------------------------------------------
// DecideOffer -- the pure "is this release offerable?" rule CheckForUpdate uses
// (ADR 0055: verification reinstall).
// ---------------------------------------------------------------------------

namespace {

CheckParams NormalParams() {
    CheckParams p;
    p.current_version = *ParseSemVer("0.9.0-rc4");
    p.current_version_raw = "0.9.0-rc4";
    return p;
}

CheckParams VerifyParams() {
    CheckParams p = NormalParams();
    p.allow_same_version_reinstall = true;
    return p;
}

} // namespace

TEST(DecideOffer, NewerReleaseIsANormalUpdate) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0"), "0.9.0", NormalParams()), UpdateOffer::Update);
}

TEST(DecideOffer, SameVersionIsNotOfferedNormally) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-rc4"), "0.9.0-rc4", NormalParams()), UpdateOffer::None);
}

TEST(DecideOffer, OlderVersionIsNotOfferedNormally) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-rc3"), "0.9.0-rc3", NormalParams()), UpdateOffer::None);
}

TEST(DecideOffer, VerifyModeOffersTheIdenticalVersionAsAReinstall) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-rc4"), "0.9.0-rc4", VerifyParams()), UpdateOffer::VerificationReinstall);
}

TEST(DecideOffer, VerifyModeStillOffersANewerReleaseAsANormalUpdate) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0"), "0.9.0", VerifyParams()), UpdateOffer::Update);
}

TEST(DecideOffer, VerifyModeNeverOffersAnOlderRelease) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-rc3"), "0.9.0-rc3", VerifyParams()), UpdateOffer::None);
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.8.1"), "0.8.1", VerifyParams()), UpdateOffer::None);
}

// SemVer equality is NOT enough: an unknown prerelease label parses to ordinal 0,
// so "0.9.0-beta1" and "0.9.0-rc0" (and any other foreign label) compare equal.
// The reinstall gate is exact string equality precisely to keep those apart.
TEST(DecideOffer, VerifyModeRequiresExactStringEqualityNotSemVerEquality) {
    CheckParams p;
    p.current_version = *ParseSemVer("0.9.0-beta1");
    p.current_version_raw = "0.9.0-beta1";
    p.allow_same_version_reinstall = true;

    const SemVer foreign = *ParseSemVer("0.9.0-alpha7");
    ASSERT_EQ(foreign, p.current_version) << "precondition: both labels collapse to prerelease ordinal 0";
    EXPECT_EQ(DecideOffer(foreign, "0.9.0-alpha7", p), UpdateOffer::None);
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-beta1"), "0.9.0-beta1", p), UpdateOffer::VerificationReinstall);
}

TEST(DecideOffer, VerifyModeWithoutARawCurrentVersionOffersNothing) {
    CheckParams p = VerifyParams();
    p.current_version_raw.clear();
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-rc4"), "0.9.0-rc4", p), UpdateOffer::None);
}

TEST(DecideOffer, VerifyModeWithoutAReleaseTagOffersNothing) {
    EXPECT_EQ(DecideOffer(*ParseSemVer("0.9.0-rc4"), "", VerifyParams()), UpdateOffer::None);
}
