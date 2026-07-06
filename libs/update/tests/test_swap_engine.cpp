#include <gtest/gtest.h>
#include <update/swap_engine.h>
#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include <fstream>
#include <windows.h> // ::GetCurrentProcessId in the process-wait case
namespace fs = std::filesystem;
using namespace exosnap::update;

namespace {
struct SwapFixture : ::testing::Test {
    fs::path root, install, staging;
    SwapPlan plan;
    void SetUp() override {
        root = fs::temp_directory_path() / "exosnap_swap_test";
        fs::remove_all(root);
        install = root / "ExoSnap";
        fs::create_directories(install);
        std::ofstream(install / "exosnap.exe") << "old";
        std::ofstream(install / "data.dll") << "old-dll";
        plan = MakeSwapPlan(install.wstring(), SemVer{0, 9, 0});
        staging = fs::path(plan.staging_dir);
        fs::create_directories(staging);
        std::ofstream(staging / "exosnap.exe") << "new";
    }
    void TearDown() override {
        fs::remove_all(root);
    }
};
} // namespace

TEST_F(SwapFixture, MakeSwapPlanDerivesSiblingDirs) {
    EXPECT_EQ(plan.staging_dir, (root / "ExoSnap.new").wstring());
    EXPECT_EQ(plan.backup_dir, (root / "ExoSnap.old").wstring());
}

TEST_F(SwapFixture, StageRenameSwapsAndKeepsBackup) {
    EXPECT_EQ(StageRename(plan), SwapError::None);
    std::ifstream live(install / "exosnap.exe");
    std::string s((std::istreambuf_iterator<char>(live)), {});
    EXPECT_EQ(s, "new");
    EXPECT_TRUE(fs::exists(fs::path(plan.backup_dir) / "data.dll"));
}

TEST_F(SwapFixture, StagingWithoutExeIsRejectedUntouched) {
    fs::remove(staging / "exosnap.exe");
    EXPECT_EQ(StageRename(plan), SwapError::StagingMissing);
    EXPECT_TRUE(fs::exists(install / "exosnap.exe")); // old install untouched
}

TEST_F(SwapFixture, RestoreBringsOldVersionBack) {
    ASSERT_EQ(StageRename(plan), SwapError::None);
    EXPECT_EQ(RestoreBackup(plan), SwapError::None);
    std::ifstream live(install / "exosnap.exe");
    std::string s((std::istreambuf_iterator<char>(live)), {});
    EXPECT_EQ(s, "old");
}

TEST_F(SwapFixture, CleanupRemovesBackup) {
    ASSERT_EQ(StageRename(plan), SwapError::None);
    EXPECT_TRUE(CleanupBackup(plan));
    EXPECT_FALSE(fs::exists(plan.backup_dir));
}

TEST(SwapEngine, ReadFileVersionOnSystemDllAndGarbage) {
    EXPECT_TRUE(ReadFileVersion(L"C:\\Windows\\System32\\kernel32.dll").has_value());
    auto txt = fs::temp_directory_path() / "exosnap_notanexe.txt";
    std::ofstream(txt) << "hi";
    EXPECT_FALSE(ReadFileVersion(txt.wstring()).has_value());
    fs::remove(txt);
}

TEST(SwapEngine, WaitForProcessExitHandlesGoneAndSelf) {
    EXPECT_TRUE(WaitForProcessExit(0, std::chrono::milliseconds(10))); // pid 0 = not running
    EXPECT_FALSE(WaitForProcessExit(::GetCurrentProcessId(), std::chrono::milliseconds(50)));
}
