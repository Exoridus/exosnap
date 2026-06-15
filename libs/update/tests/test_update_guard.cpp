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
