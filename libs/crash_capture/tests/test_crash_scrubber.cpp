// test_crash_scrubber.cpp — Unit tests for the crash-capture scrubber.
//
// These tests exercise pure string scrubbing logic.  No sentry-native or
// Windows process launch is required.  All tests pass in the skeleton build
// (EXOSNAP_CRASH_CAPTURE_AVAILABLE not defined).

#include <crash_capture/crash_scrubber.h>

#include <array>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace exosnap::crash_capture;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// Inject a known user/machine context so tests are reproducible on any machine.
struct ScrubberTestFixture : ::testing::Test {
    void SetUp() override {
        SensitiveValueCache::Reset();
        auto& cache = SensitiveValueCache::Instance();
        cache.userprofile = "C:\\Users\\TestUser";
        cache.username = "TestUser";
        cache.machine_name = "DESKTOP-TESTBOX";
    }
    void TearDown() override {
        SensitiveValueCache::Reset();
    }
};

// ---------------------------------------------------------------------------
// ScrubString tests
// ---------------------------------------------------------------------------

TEST_F(ScrubberTestFixture, EmptyStringReturnedUnchanged) {
    EXPECT_EQ(ScrubString(""), "");
}

TEST_F(ScrubberTestFixture, PlainTextUnchanged) {
    EXPECT_EQ(ScrubString("AV1 encoder init failed"), "AV1 encoder init failed");
}

TEST_F(ScrubberTestFixture, UserProfilePathStripped) {
    std::string input = "Recording saved to C:\\Users\\TestUser\\Videos\\clip.mkv";
    std::string result = ScrubString(input);
    EXPECT_EQ(result.find("TestUser"), std::string::npos)
        << "Username must not appear in scrubbed output; got: " << result;
    EXPECT_EQ(result.find("C:\\Users\\TestUser"), std::string::npos)
        << "USERPROFILE path must not appear in scrubbed output; got: " << result;
}

TEST_F(ScrubberTestFixture, GenericWindowsPathStripped) {
    std::string input = "Output path: C:\\ProgramFiles\\ExoSnap\\something.mkv";
    std::string result = ScrubString(input);
    EXPECT_EQ(result.find("C:\\ProgramFiles"), std::string::npos)
        << "Absolute Windows path must be stripped; got: " << result;
    EXPECT_NE(result.find("[path]"), std::string::npos) << "Replacement placeholder must be present; got: " << result;
}

TEST_F(ScrubberTestFixture, UncPathStripped) {
    std::string input = "File at \\\\server\\share\\video.mkv was not writable";
    std::string result = ScrubString(input);
    EXPECT_EQ(result.find("\\\\server"), std::string::npos) << "UNC path must be stripped; got: " << result;
}

TEST_F(ScrubberTestFixture, UsernameStrippedStandalone) {
    std::string input = "User TestUser started a recording session";
    std::string result = ScrubString(input);
    EXPECT_EQ(result.find("TestUser"), std::string::npos) << "Standalone username must be stripped; got: " << result;
    EXPECT_NE(result.find("[user]"), std::string::npos) << "User placeholder must appear; got: " << result;
}

TEST_F(ScrubberTestFixture, MachineNameStripped) {
    std::string input = "Encoder initialized on DESKTOP-TESTBOX with AV1";
    std::string result = ScrubString(input);
    EXPECT_EQ(result.find("DESKTOP-TESTBOX"), std::string::npos) << "Machine name must be stripped; got: " << result;
    EXPECT_NE(result.find("[machine]"), std::string::npos) << "Machine placeholder must appear; got: " << result;
}

TEST_F(ScrubberTestFixture, AllowedTechnicalTermsPreserved) {
    std::string input = "AV1 P4 preset encoder backend nvenc container mkv";
    std::string result = ScrubString(input);
    EXPECT_NE(result.find("AV1"), std::string::npos);
    EXPECT_NE(result.find("nvenc"), std::string::npos);
    EXPECT_NE(result.find("mkv"), std::string::npos);
}

TEST_F(ScrubberTestFixture, CaseInsensitiveUsernameMatch) {
    std::string input = "Failed to open C:\\users\\testuser\\clip.mkv";
    std::string result = ScrubString(input);
    EXPECT_EQ(result.find("testuser"), std::string::npos)
        << "Case-insensitive username match must strip the value; got: " << result;
}

// ---------------------------------------------------------------------------
// IsAllowedTagKey tests
// ---------------------------------------------------------------------------

TEST(IsAllowedTagKeyTest, AllowedKeysPass) {
    EXPECT_TRUE(IsAllowedTagKey("os.name"));
    EXPECT_TRUE(IsAllowedTagKey("os.version"));
    EXPECT_TRUE(IsAllowedTagKey("gpu.model"));
    EXPECT_TRUE(IsAllowedTagKey("gpu.vendor"));
    EXPECT_TRUE(IsAllowedTagKey("gpu.driver"));
    EXPECT_TRUE(IsAllowedTagKey("app.version"));
    EXPECT_TRUE(IsAllowedTagKey("encoder_backend"));
    EXPECT_TRUE(IsAllowedTagKey("container"));
    EXPECT_TRUE(IsAllowedTagKey("video_codec"));
    EXPECT_TRUE(IsAllowedTagKey("audio_codec"));
}

TEST(IsAllowedTagKeyTest, DeniedKeysBlocked) {
    EXPECT_FALSE(IsAllowedTagKey("user"));
    EXPECT_FALSE(IsAllowedTagKey("username"));
    EXPECT_FALSE(IsAllowedTagKey("machine"));
    EXPECT_FALSE(IsAllowedTagKey("output_path"));
    EXPECT_FALSE(IsAllowedTagKey("recording_filename"));
    EXPECT_FALSE(IsAllowedTagKey("install_id"));
    EXPECT_FALSE(IsAllowedTagKey("")); // empty key is denied
}

// ---------------------------------------------------------------------------
// GenerateCorrelationId tests
// ---------------------------------------------------------------------------

TEST(GenerateCorrelationIdTest, CorrectUuidV4Format) {
    std::string id = GenerateCorrelationId();
    // UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx (36 chars)
    ASSERT_EQ(id.size(), 36u);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');
    EXPECT_EQ(id[14], '4'); // version nibble
}

TEST(GenerateCorrelationIdTest, UniquePerCall) {
    auto id1 = GenerateCorrelationId();
    auto id2 = GenerateCorrelationId();
    EXPECT_NE(id1, id2) << "Correlation IDs must be unique per call";
}

// ---------------------------------------------------------------------------
// Allowlist Golden-Set test (ADR 0045 / D2).
//
// kAllowedTagKeys now lives ONCE, in crash_scrubber.h (previously it was also
// hand-repeated as a literal brace-list inside crash_capture.cpp's
// BeforeSendHook — that copy is gone; the hook now iterates AllowedTagKeys()).
// This test hardcodes an independent "golden" copy of the expected key set so
// that ANY change to the allowlist — adding, removing, renaming a key — fails
// this test and forces the author to consciously update the golden array
// here, which is the trigger to also update the PRIVACY.md / product-spec
// §14 mapping table (scripts/validate-privacy-allowlist.ps1 enforces that
// those documents list exactly this set).
// ---------------------------------------------------------------------------

TEST(AllowedTagKeysGoldenTest, MatchesGoldenSet) {
    // Golden set: the fields the app promises are the ONLY structured tags
    // that survive BeforeSendHook. Keep this list and PRIVACY.md's mapping
    // table in lockstep by hand; the CI script cross-checks them.
    static constexpr std::array<std::string_view, 10> kGolden = {
        "os.name",     "os.version",      "gpu.model", "gpu.vendor",  "gpu.driver",
        "app.version", "encoder_backend", "container", "video_codec", "audio_codec",
    };

    const auto actual = AllowedTagKeys();
    ASSERT_EQ(actual.size(), kGolden.size()) << "kAllowedTagKeys size drifted from the golden set — update both "
                                                "this test and the PRIVACY.md / product-spec §14 mapping table";
    for (size_t i = 0; i < kGolden.size(); ++i) {
        EXPECT_EQ(actual[i], kGolden[i]) << "kAllowedTagKeys[" << i << "] drifted from the golden set";
    }
}

TEST(AllowedTagKeysGoldenTest, EveryGoldenKeyIsAllowed) {
    // Cross-check against IsAllowedTagKey so the two never silently diverge
    // (this would only be possible with a hand-edit bug in IsAllowedTagKey).
    static constexpr std::array<std::string_view, 10> kGolden = {
        "os.name",     "os.version",      "gpu.model", "gpu.vendor",  "gpu.driver",
        "app.version", "encoder_backend", "container", "video_codec", "audio_codec",
    };
    for (const auto& key : kGolden) {
        EXPECT_TRUE(IsAllowedTagKey(key)) << "golden key not accepted by IsAllowedTagKey: " << key;
    }
}

TEST(GenerateCorrelationIdTest, NotPersistentAcrossCalls) {
    // Simply verify that calling multiple times produces different results
    // (no singleton / no file persistence)
    std::string ids[5];
    for (auto& id : ids)
        id = GenerateCorrelationId();
    for (size_t i = 0; i < 4; ++i)
        EXPECT_NE(ids[i], ids[i + 1]);
}
