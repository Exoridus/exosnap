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

TEST_F(SwapFixture, MakeSwapPlanTrimsTrailingSeparator) {
    // The MSI's [INSTALLFOLDER] always ends with a backslash. A trailing '\' or
    // '/' must be trimmed so the siblings land *beside* the install dir, not
    // inside it.
    const SwapPlan a = MakeSwapPlan(install.wstring() + L"\\", SemVer{0, 9, 0});
    const SwapPlan b = MakeSwapPlan(install.wstring() + L"/", SemVer{0, 9, 0});
    EXPECT_EQ(a.install_dir, plan.install_dir);
    EXPECT_EQ(a.staging_dir, plan.staging_dir);
    EXPECT_EQ(a.backup_dir, plan.backup_dir);
    EXPECT_EQ(b.install_dir, plan.install_dir);
    EXPECT_EQ(b.staging_dir, plan.staging_dir);
    EXPECT_EQ(b.backup_dir, plan.backup_dir);
}

TEST_F(SwapFixture, StageRenameSwapsAndKeepsBackup) {
    EXPECT_EQ(StageRename(plan), SwapError::None);
    std::ifstream live(install / "exosnap.exe");
    std::string s((std::istreambuf_iterator<char>(live)), {});
    EXPECT_EQ(s, "new");
    EXPECT_TRUE(fs::exists(fs::path(plan.backup_dir) / "data.dll"));
}

TEST_F(SwapFixture, StageRenameCompensatesWhenPromoteFails) {
    // Hold an open handle on the staging exe. MSVC's std::ifstream opens without
    // FILE_SHARE_DELETE, so the staging->install directory rename (rename 2)
    // fails with a sharing violation, while install->backup (rename 1) and the
    // compensating backup->install both succeed. The engine must roll back and
    // report RenameNewFailed with the old install live again and no backup left.
    std::ifstream lock(staging / "exosnap.exe");
    ASSERT_TRUE(lock.is_open());

    EXPECT_EQ(StageRename(plan), SwapError::RenameNewFailed);

    std::ifstream live(install / "exosnap.exe");
    std::string s((std::istreambuf_iterator<char>(live)), {});
    EXPECT_EQ(s, "old");                       // old version restored and live again
    EXPECT_FALSE(fs::exists(plan.backup_dir)); // backup was renamed back
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

TEST(SwapEngine, WaitForInstanceMutexDetectsPresentAndAbsent) {
    // A mutex we create in-process must be detected as present, and quickly.
    const std::wstring name = L"ExoSnapTestMutex_" + std::to_wstring(::GetCurrentProcessId());
    HANDLE m = ::CreateMutexW(nullptr, FALSE, name.c_str());
    ASSERT_NE(m, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(WaitForInstanceMutex(name.c_str(), std::chrono::milliseconds(2000)));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(500));
    ::CloseHandle(m);

    // A name that was never created must never be reported present; the call
    // polls until the (short) timeout and then returns false.
    const std::wstring absent = L"ExoSnapTestMutex_never_" + std::to_wstring(::GetCurrentProcessId());
    EXPECT_FALSE(WaitForInstanceMutex(absent.c_str(), std::chrono::milliseconds(300)));
}
