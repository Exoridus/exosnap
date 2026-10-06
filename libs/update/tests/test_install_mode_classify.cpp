#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <update/install_mode_detector.h>
#include <update/update_types.h>
#include <utility>

#include "../src/install_mode_classify.h"

using exosnap::update::ClassifyInstallMode;
using exosnap::update::InstallMode;
using exosnap::update::InstallStamp;
using exosnap::update::NormalizeDirForCompare;

namespace {

// A stamp as one hive actually carries it: the marker is the stamp's existence,
// the path is its content.
InstallStamp Stamp(std::wstring install_dir) {
    InstallStamp stamp;
    stamp.install_dir = std::move(install_dir);
    return stamp;
}

} // namespace

TEST(NormalizeDirForCompare, IgnoresCaseTrailingSeparatorAndSlashDirection) {
    EXPECT_EQ(NormalizeDirForCompare(L"C:\\Program Files\\Codexo\\ExoSnap\\"),
              NormalizeDirForCompare(L"c:/program files/codexo/exosnap"));
}

TEST(ClassifyInstallMode, NoStampIsPortable) {
    EXPECT_EQ(ClassifyInstallMode(std::nullopt, L"C:\\rc\\rc17"), InstallMode::Portable);
    // Even standing in the install directory: without a stamp nothing installed it.
    EXPECT_EQ(ClassifyInstallMode(std::nullopt, L"C:\\Program Files\\Codexo\\ExoSnap"), InstallMode::Portable);
}

TEST(ClassifyInstallMode, StampMatchingOwnDirectoryIsInstalled) {
    EXPECT_EQ(ClassifyInstallMode(Stamp(L"C:\\Program Files\\Codexo\\ExoSnap"), L"C:\\Program Files\\Codexo\\ExoSnap"),
              InstallMode::Installed);
    EXPECT_EQ(ClassifyInstallMode(Stamp(L"C:\\Program Files\\Codexo\\ExoSnap\\"), L"c:/program files/codexo/exosnap"),
              InstallMode::Installed);
}

// The defect this function exists for: a portable copy on a machine that also
// carries an MSI install inherits the machine-wide marker, and without the
// directory comparison it claims to be the installed copy -- which the updater
// then refuses as a registry mismatch, leaving it unable to update itself.
TEST(ClassifyInstallMode, StampForADifferentDirectoryIsPortable) {
    EXPECT_EQ(ClassifyInstallMode(Stamp(L"C:\\Program Files\\Codexo\\ExoSnap"),
                                  L"C:\\rc\\rc17\\ExoSnap-0.9.0-rc17-windows-x64-portable"),
              InstallMode::Portable);
}

// A stamp with no InstallPath proves only that SOME install exists on this
// machine, never that this copy is it -- exactly the state the directory
// comparison exists to reject. The MSI writes "installed" and "InstallPath" in
// one component under one key, so a real install always carries both.
TEST(ClassifyInstallMode, StampWithoutAnInstallPathIsPortable) {
    EXPECT_EQ(ClassifyInstallMode(Stamp(L""), L"C:\\Program Files\\Codexo\\ExoSnap"), InstallMode::Portable);
    EXPECT_EQ(ClassifyInstallMode(Stamp(L""), L"C:\\anywhere"), InstallMode::Portable);
}

// Not knowing where this executable lives is not evidence of being the install.
// Portable costs a directory rename that fails honestly against a real
// installation; the other answer would run msiexec on behalf of a copy that
// could not show it is the installed one.
TEST(ClassifyInstallMode, UnknownOwnDirectoryIsPortable) {
    EXPECT_EQ(ClassifyInstallMode(Stamp(L"C:\\Program Files\\Codexo\\ExoSnap"), L""), InstallMode::Portable);
}

TEST(ClassifyDistributionContext, InstalledOwnerMarkerIsScopedToMatchingRecord) {
    using namespace exosnap::update;
    struct Case {
        std::optional<std::wstring> marker;
        DistributionOwner expected;
    };
    const Case cases[] = {
        {std::nullopt, DistributionOwner::Direct},       {L"direct", DistributionOwner::Direct},
        {L"winget", DistributionOwner::WinGet},          {L"chocolatey", DistributionOwner::Chocolatey},
        {L"unknown", DistributionOwner::UnknownManaged}, {L"", DistributionOwner::UnknownManaged},
        {L"scoop", DistributionOwner::UnknownManaged},   {L"WinGet", DistributionOwner::UnknownManaged},
        {L"other", DistributionOwner::UnknownManaged},
    };
    for (const auto& item : cases) {
        InstallStamp stamp{L"C:\\Program Files\\ExoSnap", item.marker};
        EXPECT_EQ(ClassifyDistributionContext(stamp, L"c:/program files/exosnap/", true),
                  (DistributionContext{InstallMode::Installed, item.expected}));
        EXPECT_EQ(ClassifyDistributionContext(stamp, L"D:\\Portable\\ExoSnap"),
                  (DistributionContext{InstallMode::Portable, DistributionOwner::Direct}));
    }
}

TEST(ClassifyDistributionContext, ScoopLayoutsSupportUserGlobalAndRelocatedRoots) {
    using namespace exosnap::update;
    for (const auto* directory :
         {L"C:\\Users\\Person\\scoop\\apps\\exosnap\\current", L"C:\\Users\\Person\\scoop\\apps\\exosnap\\1.2.3",
          L"C:\\ProgramData\\scoop\\apps\\exosnap\\current", L"D:\\Custom\\apps\\exosnap\\current",
          L"d:/custom/apps/EXOSNAP/current/"}) {
        EXPECT_EQ(ClassifyDistributionContext(std::nullopt, directory),
                  (DistributionContext{InstallMode::Portable, DistributionOwner::Scoop}));
    }
}

TEST(ClassifyDistributionContext, AdjacentMetadataSupportsUnrelatedPortableDirectory) {
    using namespace exosnap::update;
    EXPECT_EQ(ClassifyDistributionContext(std::nullopt, L"D:\\Unrelated\\ExoSnap", true),
              (DistributionContext{InstallMode::Portable, DistributionOwner::Scoop}));
    EXPECT_EQ(ClassifyDistributionContext(Stamp(L"C:\\Program Files\\ExoSnap"), L"D:\\Unrelated\\ExoSnap", true),
              (DistributionContext{InstallMode::Portable, DistributionOwner::Scoop}));
}

TEST(ClassifyDistributionContext, SimilarPathsAndAbsentEvidenceRemainDirect) {
    using namespace exosnap::update;
    for (const auto* directory :
         {L"D:\\Portable\\ExoSnap", L"C:\\myscoop\\apps\\exosnap\\1.2.3", L"C:\\scoop\\apps-old\\exosnap\\current",
          L"C:\\scoop\\apps\\other\\current", L"C:\\scoop\\apps\\exosnap-copy\\current",
          L"D:\\Custom\\apps\\exosnap\\1.2.3", L"D:\\Custom\\apps\\exosnap\\current-copy",
          L"C:\\scoop\\apps\\exosnap\\current\\unrelated", L""}) {
        EXPECT_EQ(ClassifyDistributionContext(std::nullopt, directory),
                  (DistributionContext{InstallMode::Portable, DistributionOwner::Direct}));
    }
}

class DistributionDetectorTest : public testing::Test {
  protected:
    void SetUp() override {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() / ("exosnap_distribution_" + std::to_string(suffix));
        ASSERT_TRUE(std::filesystem::create_directory(directory_));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        EXPECT_FALSE(error);
    }

    void WriteMetadata(const char* filename, const char* content) {
        std::ofstream file(directory_ / filename);
        ASSERT_TRUE(file.good());
        file << content;
    }

    std::filesystem::path directory_;
};

TEST_F(DistributionDetectorTest, OnlyRegularAdjacentScoopMetadataClaimsPortableOwnership) {
    using namespace exosnap::update;
    EXPECT_EQ(DetectDistributionContext(directory_.wstring()),
              (DistributionContext{InstallMode::Portable, DistributionOwner::Direct}));
    for (const auto* filename : {"scoop-install.json", "scoop-manifest.json"}) {
        const auto metadata = directory_ / filename;
        ASSERT_TRUE(std::filesystem::create_directory(metadata));
        EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Direct);
        ASSERT_TRUE(std::filesystem::remove(metadata));
        {
            std::ofstream file(metadata);
            ASSERT_TRUE(file.good());
            file << "{}";
        }
        EXPECT_EQ(DetectDistributionContext(directory_.wstring()),
                  (DistributionContext{InstallMode::Portable, DistributionOwner::Scoop}));
        ASSERT_TRUE(std::filesystem::remove(metadata));
    }
}

TEST_F(DistributionDetectorTest, ParentMetadataDoesNotClaimUnrelatedChild) {
    using namespace exosnap::update;
    {
        std::ofstream file(directory_ / "scoop-install.json");
        ASSERT_TRUE(file.good());
        file << "{}";
    }
    const auto child = directory_ / "unrelated";
    ASSERT_TRUE(std::filesystem::create_directory(child));
    EXPECT_EQ(DetectDistributionContext(child.wstring()),
              (DistributionContext{InstallMode::Portable, DistributionOwner::Direct}));
}

TEST_F(DistributionDetectorTest, LegacyScoopMetadataPairSupportsBucketAndUrlInstalls) {
    using namespace exosnap::update;
    WriteMetadata("manifest.json",
                  R"({"version":"1.2.3","architecture":{"64bit":{"url":"https://example.test/exosnap.zip"}}})");
    for (const auto* install : {R"({"architecture":"64bit","bucket":"extras"})",
                                R"({"architecture":"64bit","url":"D:/manifests/exosnap.json"})"}) {
        WriteMetadata("install.json", install);
        EXPECT_EQ(DetectDistributionContext(directory_.wstring()),
                  (DistributionContext{InstallMode::Portable, DistributionOwner::Scoop}));
    }
    WriteMetadata("install.json", R"({"architecture":"arm64","bucket":"extras"})");
    WriteMetadata("manifest.json", R"({"version":"1.2.3","url":["https://example.test/exosnap.zip"]})");
    EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Scoop);
}

TEST_F(DistributionDetectorTest, PartialOrUnrelatedLegacyMetadataDoesNotClaimOwnership) {
    using namespace exosnap::update;
    WriteMetadata("install.json", R"({"architecture":"64bit","bucket":"extras"})");
    EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Direct);
    ASSERT_TRUE(std::filesystem::remove(directory_ / "install.json"));
    WriteMetadata("manifest.json", R"({"version":"1.2.3","url":"https://example.test/exosnap.zip"})");
    EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Direct);
    ASSERT_TRUE(std::filesystem::create_directory(directory_ / "install.json"));
    EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Direct);
    ASSERT_TRUE(std::filesystem::remove(directory_ / "install.json"));

    for (const auto* install :
         {"{}", "[]", "broken", R"({"architecture":"64bit"})", R"({"architecture":"x64","bucket":"extras"})",
          R"({"architecture":"64bit","bucket":"","url":5})"}) {
        WriteMetadata("install.json", install);
        EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Direct);
    }
    WriteMetadata("install.json", R"({"architecture":"64bit","bucket":"extras"})");
    for (const auto* manifest :
         {"{}", "[]", "broken", R"({"version":"1.2.3"})", R"({"version":"","url":"https://example.test/exosnap.zip"})",
          R"({"version":"1.2.3","architecture":{"32bit":{"url":"https://example.test/exosnap.zip"}}})"}) {
        WriteMetadata("manifest.json", manifest);
        EXPECT_EQ(DetectDistributionContext(directory_.wstring()).owner, DistributionOwner::Direct);
    }
}
