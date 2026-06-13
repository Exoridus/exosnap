// LOW-DISK-GUARD-R1 test suite
//
// Covers:
//   1. DiskSpaceThresholds — pure threshold constants and ComputeHardStopThreshold()
//   2. RecommendationEngine — two-tier disk check (rec.005 Notice / rec.007 Blocker)
//   3. IDiskSpaceProvider stub and Win32 provider compilation
//   4. DiagnosticChecklist flag propagation for disk checks

#include <gtest/gtest.h>

#include "diagnostics/DiagnosticResult.h"
#include "diagnostics/DiskSpaceProvider.h"
#include "diagnostics/DiskSpaceThresholds.h"
#include "diagnostics/RecommendationEngine.h"

#include <capability/capability_set.h>
#include <capability/user_config.h>

#include <filesystem>

namespace exosnap::diagnostics {
namespace {

// ─── Stub provider for controlled free-space simulation ──────────────────────

class StubDiskSpaceProvider final : public IDiskSpaceProvider {
  public:
    explicit StubDiskSpaceProvider(uint64_t free_bytes) : free_bytes_(free_bytes) {
    }

    [[nodiscard]] uint64_t FreeBytesForPath(const std::filesystem::path& /*path*/) const override {
        return free_bytes_;
    }

  private:
    uint64_t free_bytes_;
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

static capability::CapabilitySet MakeBasicCaps() {
    capability::CapabilitySet caps;
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Available, ""};
    caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    return caps;
}

static capability::UserRecorderConfig MakeBasicConfig() {
    capability::UserRecorderConfig config;
    config.container = capability::Container::Matroska;
    config.video_codec = capability::VideoCodec::Av1Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;
    return config;
}

// ─── DiskSpaceThresholds ─────────────────────────────────────────────────────

TEST(DiskSpaceThresholdsTest, HardStopThresholdIs500MB) {
    EXPECT_EQ(kHardStopFreeBytes, 500ULL * 1024 * 1024);
}

TEST(DiskSpaceThresholdsTest, WarnThresholdIs2GB) {
    EXPECT_EQ(kWarnFreeBytes, 2ULL * 1024 * 1024 * 1024);
}

TEST(DiskSpaceThresholdsTest, WarnThresholdExceedsHardStop) {
    EXPECT_GT(kWarnFreeBytes, kHardStopFreeBytes);
}

TEST(DiskSpaceThresholdsTest, ComputeHardStopThreshold_NoRemux_ReturnsBase) {
    EXPECT_EQ(ComputeHardStopThreshold(0), kHardStopFreeBytes);
}

TEST(DiskSpaceThresholdsTest, ComputeHardStopThreshold_WithRemuxReserve_AddsToBase) {
    const uint64_t reserve = 1ULL * 1024 * 1024 * 1024; // 1 GB transient MKV
    EXPECT_EQ(ComputeHardStopThreshold(reserve), kHardStopFreeBytes + reserve);
}

TEST(DiskSpaceThresholdsTest, ComputeHardStopThreshold_LargeReserve) {
    // 10 GB recording → threshold = 500 MB + 10 GB
    const uint64_t reserve = 10ULL * 1024 * 1024 * 1024;
    const uint64_t expected = kHardStopFreeBytes + reserve;
    EXPECT_EQ(ComputeHardStopThreshold(reserve), expected);
}

// ─── RecommendationEngine — no disk-space entries when value is 0 ─────────────

TEST(LowDiskGuardRecommendationTest, ZeroFreeBytes_NoDiskEntries) {
    // 0 means "not queried"; must not produce any disk-space result.
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, /*output_drive_free_bytes=*/0, true);
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.005") << "rec.005 must not fire when free_bytes=0";
        EXPECT_NE(r.id, "rec.007") << "rec.007 must not fire when free_bytes=0";
    }
    EXPECT_FALSE(cl.has_blocker);
}

// ─── rec.005 — soft Notice tier ───────────────────────────────────────────────

TEST(LowDiskGuardRecommendationTest, WarnTier_BelowWarnAboveHardStop_Notice) {
    // 1 GB free: above hard-stop (500 MB) but below warn (2 GB) → rec.005 Notice
    const uint64_t free = 1ULL * 1024 * 1024 * 1024; // 1 GB
    ASSERT_GT(free, kHardStopFreeBytes);
    ASSERT_LT(free, kWarnFreeBytes);

    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec005 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.005") {
            found_rec005 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
        }
        EXPECT_NE(r.id, "rec.007") << "rec.007 must not fire above hard-stop threshold";
    }
    EXPECT_TRUE(found_rec005);
    EXPECT_TRUE(cl.has_notice);
    EXPECT_FALSE(cl.has_blocker);
}

TEST(LowDiskGuardRecommendationTest, WarnTier_ExactlyOneByteAboveHardStop_Notice) {
    const uint64_t free = kHardStopFreeBytes + 1;
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec005 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.005") {
            found_rec005 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
        }
    }
    EXPECT_TRUE(found_rec005);
    EXPECT_FALSE(cl.has_blocker);
}

TEST(LowDiskGuardRecommendationTest, WarnTier_ExactlyAtWarnBoundary_NoNotice) {
    // Exactly at kWarnFreeBytes → neither rec.005 nor rec.007
    const uint64_t free = kWarnFreeBytes;
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.005");
        EXPECT_NE(r.id, "rec.007");
    }
    EXPECT_FALSE(cl.has_blocker);
}

// ─── rec.007 — hard-stop Blocker tier ────────────────────────────────────────

TEST(LowDiskGuardRecommendationTest, BlockerTier_AtHardStopThreshold_Blocker) {
    // Exactly at kHardStopFreeBytes → rec.007 Blocker
    const uint64_t free = kHardStopFreeBytes;
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec007 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.007") {
            found_rec007 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
        }
        EXPECT_NE(r.id, "rec.005") << "rec.005 must not fire alongside rec.007";
    }
    EXPECT_TRUE(found_rec007);
    EXPECT_TRUE(cl.has_blocker);
    EXPECT_FALSE(cl.has_notice) << "Notice flag must not be set when blocker fires";
}

TEST(LowDiskGuardRecommendationTest, BlockerTier_BelowHardStop_Blocker) {
    // 100 MB free → well below hard-stop → rec.007 Blocker
    const uint64_t free = 100ULL * 1024 * 1024;
    ASSERT_LT(free, kHardStopFreeBytes);

    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec007 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.007") {
            found_rec007 = true;
        }
    }
    EXPECT_TRUE(found_rec007);
    EXPECT_TRUE(cl.has_blocker);
}

TEST(LowDiskGuardRecommendationTest, BlockerTier_ZeroRemaining_Blocker) {
    // Pathological: no free space at all (but value > 0 means it was queried).
    // We use 1 byte to avoid the "not queried" guard.
    const uint64_t free = 1;
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec007 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.007")
            found_rec007 = true;
    }
    EXPECT_TRUE(found_rec007);
    EXPECT_TRUE(cl.has_blocker);
}

TEST(LowDiskGuardRecommendationTest, AboveWarnThreshold_NoDiskResults) {
    // 5 GB free → above warn threshold → no disk results
    const uint64_t free = 5ULL * 1024 * 1024 * 1024;
    ASSERT_GT(free, kWarnFreeBytes);

    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.005");
        EXPECT_NE(r.id, "rec.007");
    }
    EXPECT_FALSE(cl.has_blocker);
}

// ─── rec.007 blocks even when codec/profile checks pass ──────────────────────

TEST(LowDiskGuardRecommendationTest, BlockerTier_DoesNotSuppressCodecBlocker) {
    // Both a codec blocker (rec.003) and a disk blocker (rec.007) can coexist.
    capability::CapabilitySet caps = MakeBasicCaps();
    caps.video_codecs[capability::VideoCodec::Av1Nvenc] = {capability::SupportLevel::Invalid, "no GPU"};

    capability::UserRecorderConfig config = MakeBasicConfig();

    const uint64_t free = 100ULL * 1024 * 1024; // below hard-stop

    RecommendationEngine engine(caps, config, 0, free, true);
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec003 = false, found_rec007 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.003")
            found_rec003 = true;
        if (r.id == "rec.007")
            found_rec007 = true;
    }
    EXPECT_TRUE(found_rec003);
    EXPECT_TRUE(found_rec007);
    EXPECT_TRUE(cl.has_blocker);
}

// ─── GetAllRecommendationCodes includes rec.007 ───────────────────────────────

TEST(LowDiskGuardRecommendationTest, GetAllRecommendationCodes_IncludesRec007) {
    const auto codes = RecommendationEngine::GetAllRecommendationCodes();
    const bool has_rec007 = std::find(codes.begin(), codes.end(), "rec.007") != codes.end();
    EXPECT_TRUE(has_rec007);
    // rec.005 must still be present (soft-warn tier retained)
    const bool has_rec005 = std::find(codes.begin(), codes.end(), "rec.005") != codes.end();
    EXPECT_TRUE(has_rec005);
}

// ─── IDiskSpaceProvider stub usable in tests ──────────────────────────────────

TEST(DiskSpaceProviderTest, Stub_ReturnsConfiguredValue) {
    const uint64_t expected = 3ULL * 1024 * 1024 * 1024;
    StubDiskSpaceProvider stub(expected);
    EXPECT_EQ(stub.FreeBytesForPath(std::filesystem::path(L"C:\\fake\\path")), expected);
}

TEST(DiskSpaceProviderTest, Stub_ZeroReturnsMeansNotQueried) {
    StubDiskSpaceProvider stub(0);
    EXPECT_EQ(stub.FreeBytesForPath(std::filesystem::path(L"C:\\whatever")), 0u);
}

// ─── Win32DiskSpaceProvider — compile-time check ─────────────────────────────
// (We cannot run a live query in a unit test without touching real storage,
//  but we verify the class compiles and the method can be called without
//  crashing when given a path that may or may not exist on the test host.)

TEST(DiskSpaceProviderTest, Win32Provider_ReturnsNonNegativeOnExistingRoot) {
    Win32DiskSpaceProvider provider;
    // C:\ must always exist on a Windows test host.
    const uint64_t free = provider.FreeBytesForPath(std::filesystem::path(L"C:\\"));
    // We cannot assert a specific value, but it must not throw or return a
    // nonsensical value (ULARGE_INTEGER wraps to a huge number if misused).
    // Simply verify it compiled and ran without crashing.
    (void)free;
    SUCCEED();
}

TEST(DiskSpaceProviderTest, Win32Provider_ReturnsZeroOnEmptyPath) {
    Win32DiskSpaceProvider provider;
    EXPECT_EQ(provider.FreeBytesForPath(std::filesystem::path{}), 0u);
}

} // namespace
} // namespace exosnap::diagnostics
