// FILESYSTEM-CHECKS-R1 test suite
//
// Covers:
//   1. IFilesystemProvider stub — injectable simulation of filesystem names
//   2. Win32FilesystemProvider — compile + smoke (real query on C:\)
//   3. RecommendationEngine — rec.008 FAT32 warning (Notice, not Blocker)
//   4. NTFS / exFAT / unknown pass silently (no rec.008)
//   5. Empty filesystem name suppresses the check (no false positives)
//   6. GetAllRecommendationCodes includes rec.008
//   7. Interaction: rec.008 can coexist with other checks (e.g. rec.005)

#include <gtest/gtest.h>

#include "diagnostics/DiagnosticResult.h"
#include "diagnostics/DiskSpaceThresholds.h"
#include "diagnostics/FilesystemProvider.h"
#include "diagnostics/RecommendationEngine.h"

#include <capability/capability_set.h>
#include <capability/user_config.h>

#include <filesystem>

namespace exosnap::diagnostics {
namespace {

// ─── Stub provider for controlled filesystem simulation ──────────────────────

class StubFilesystemProvider final : public IFilesystemProvider {
  public:
    explicit StubFilesystemProvider(std::string name) : name_(std::move(name)) {
    }

    [[nodiscard]] std::string FilesystemNameForPath(const std::filesystem::path& /*path*/) const override {
        return name_;
    }

  private:
    std::string name_;
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

// ─── IFilesystemProvider stub ─────────────────────────────────────────────────

TEST(FilesystemProviderTest, Stub_ReturnsConfiguredName) {
    StubFilesystemProvider stub("FAT32");
    EXPECT_EQ(stub.FilesystemNameForPath(std::filesystem::path(L"D:\\Recordings")), "FAT32");
}

TEST(FilesystemProviderTest, Stub_EmptyName_ReturnsEmpty) {
    StubFilesystemProvider stub("");
    EXPECT_EQ(stub.FilesystemNameForPath(std::filesystem::path(L"C:\\fake")), "");
}

TEST(FilesystemProviderTest, Stub_Ntfs_ReturnsNtfs) {
    StubFilesystemProvider stub("NTFS");
    EXPECT_EQ(stub.FilesystemNameForPath(std::filesystem::path(L"C:\\")), "NTFS");
}

// ─── Win32FilesystemProvider — smoke test ─────────────────────────────────────
// We cannot control which filesystem C:\ uses, but we verify the class compiles
// and returns a non-empty string for a path that is guaranteed to exist.

TEST(FilesystemProviderTest, Win32Provider_ReturnsNonEmptyOnExistingRoot) {
    Win32FilesystemProvider provider;
    // C:\ always exists on a Windows test host.
    const std::string name = provider.FilesystemNameForPath(std::filesystem::path(L"C:\\"));
    // Must not be empty — NTFS is virtually guaranteed on a dev machine.
    EXPECT_FALSE(name.empty());
}

TEST(FilesystemProviderTest, Win32Provider_ReturnsEmptyOnEmptyPath) {
    Win32FilesystemProvider provider;
    EXPECT_TRUE(provider.FilesystemNameForPath(std::filesystem::path{}).empty());
}

// ─── rec.008 — FAT32 produces a Notice ───────────────────────────────────────

TEST(FilesystemChecksRecommendationTest, Fat32_ProducesNotice) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, 0, true, "FAT32");
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec008 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.008") {
            found_rec008 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice);
            EXPECT_EQ(r.group, DiagnosticGroup::Recommendation);
            // Must mention FAT32 and 4 GiB in the texts.
            EXPECT_NE(r.title.find("FAT32"), std::string::npos);
            EXPECT_NE(r.summary.find("4 GiB"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_rec008);
    EXPECT_TRUE(cl.has_notice);
    EXPECT_FALSE(cl.has_blocker) << "FAT32 must be Notice, not Blocker";
}

// ─── rec.008 — NTFS passes silently ─────────────────────────────────────────

TEST(FilesystemChecksRecommendationTest, Ntfs_NoRec008) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS");
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.008") << "NTFS must not trigger rec.008";
    }
}

// ─── rec.008 — exFAT passes silently ─────────────────────────────────────────

TEST(FilesystemChecksRecommendationTest, ExFat_NoRec008) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, 0, true, "exFAT");
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.008") << "exFAT must not trigger rec.008";
    }
}

// ─── rec.008 — unknown filesystem passes silently ────────────────────────────

TEST(FilesystemChecksRecommendationTest, UnknownFilesystem_NoRec008) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    // "ReFS" is a real Windows filesystem; we want it to pass without a warning.
    RecommendationEngine engine(caps, config, 0, 0, true, "ReFS");
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.008") << "ReFS (unknown) must not trigger rec.008";
    }
}

// ─── rec.008 — empty name suppresses the check ───────────────────────────────

TEST(FilesystemChecksRecommendationTest, EmptyFilesystemName_NoRec008) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    RecommendationEngine engine(caps, config, 0, 0, true, "");
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.008") << "rec.008 must not fire when filesystem name is empty";
    }
}

// ─── rec.008 — default (no 6th arg) suppresses the check ─────────────────────

TEST(FilesystemChecksRecommendationTest, DefaultFilesystemName_NoRec008) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    // Constructor with only 5 args — filesystem_name defaults to empty string.
    RecommendationEngine engine(caps, config, 0, 0, true);
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.008") << "rec.008 must not fire when filesystem name is not supplied";
    }
}

// ─── rec.008 coexists with rec.005 (disk-space warning) ──────────────────────

TEST(FilesystemChecksRecommendationTest, Fat32_Plus_LowDisk_BothFire) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    // 1 GB free → rec.005 Notice; FAT32 → rec.008 Notice.
    const uint64_t free = 1ULL * 1024 * 1024 * 1024;
    ASSERT_GT(free, kHardStopFreeBytes);
    ASSERT_LT(free, kWarnFreeBytes);

    RecommendationEngine engine(caps, config, 0, free, true, "FAT32");
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec005 = false;
    bool found_rec008 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.005")
            found_rec005 = true;
        if (r.id == "rec.008")
            found_rec008 = true;
    }
    EXPECT_TRUE(found_rec005);
    EXPECT_TRUE(found_rec008);
    EXPECT_TRUE(cl.has_notice);
    EXPECT_FALSE(cl.has_blocker);
}

// ─── rec.008 does not promote to Blocker even alongside rec.007 ──────────────

TEST(FilesystemChecksRecommendationTest, Fat32_Plus_HardDiskStop_Fat32RemainsNotice) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    // Well below 500 MB → rec.007 Blocker; FAT32 → rec.008 Notice.
    const uint64_t free = 100ULL * 1024 * 1024;
    ASSERT_LT(free, kHardStopFreeBytes);

    RecommendationEngine engine(caps, config, 0, free, true, "FAT32");
    const DiagnosticChecklist cl = engine.Generate();

    bool found_rec007 = false;
    bool found_rec008 = false;
    for (const auto& r : cl.results) {
        if (r.id == "rec.007") {
            found_rec007 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Blocker);
        }
        if (r.id == "rec.008") {
            found_rec008 = true;
            EXPECT_EQ(r.severity, DiagnosticSeverity::Notice)
                << "rec.008 severity must stay Notice even when rec.007 Blocker is also present";
        }
    }
    EXPECT_TRUE(found_rec007);
    EXPECT_TRUE(found_rec008);
    EXPECT_TRUE(cl.has_blocker);
    EXPECT_TRUE(cl.has_notice);
}

// ─── GetAllRecommendationCodes includes rec.008 ───────────────────────────────

TEST(FilesystemChecksRecommendationTest, GetAllRecommendationCodes_IncludesRec008) {
    const auto codes = RecommendationEngine::GetAllRecommendationCodes();
    // FILESYSTEM-CHECKS-R1 added rec.008 — expect 8 codes now.
    EXPECT_EQ(codes.size(), 8u);
    const bool has_rec008 = std::find(codes.begin(), codes.end(), "rec.008") != codes.end();
    EXPECT_TRUE(has_rec008);
}

// ─── FAT32 case-sensitivity guard ────────────────────────────────────────────
// Windows returns "FAT32" in upper-case; ensure we don't accidentally fire on
// lower-case or mixed-case variants that some stub might produce.

TEST(FilesystemChecksRecommendationTest, LowerCaseFat32_NoRec008) {
    const capability::CapabilitySet caps = MakeBasicCaps();
    const capability::UserRecorderConfig config = MakeBasicConfig();

    // "fat32" is not what GetVolumeInformationW returns; must not trigger rec.008.
    RecommendationEngine engine(caps, config, 0, 0, true, "fat32");
    const DiagnosticChecklist cl = engine.Generate();

    for (const auto& r : cl.results) {
        EXPECT_NE(r.id, "rec.008") << "rec.008 must only fire for exact \"FAT32\" string";
    }
}

} // namespace
} // namespace exosnap::diagnostics
