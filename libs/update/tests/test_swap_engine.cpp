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

// ---------------------------------------------------------------------------
// RepairOrphanedSwap -- heals a StageRename interrupted between its two
// renames (install_dir gone, backup_dir still holding the last-known-good
// tree).
// ---------------------------------------------------------------------------

TEST_F(SwapFixture, RepairOrphanedSwapIsNoOpWhenInstallIsIntact) {
    EXPECT_TRUE(RepairOrphanedSwap(plan));
    std::ifstream live(install / "exosnap.exe");
    std::string s((std::istreambuf_iterator<char>(live)), {});
    EXPECT_EQ(s, "old"); // untouched -- the fixture's original install
}

TEST_F(SwapFixture, RepairOrphanedSwapIsNoOpWhenNoBackupExists) {
    fs::remove_all(install); // install missing, but nothing to restore from either
    EXPECT_TRUE(RepairOrphanedSwap(plan));
    EXPECT_FALSE(fs::exists(install));
}

TEST_F(SwapFixture, RepairOrphanedSwapRestoresFromBackupWhenInstallIsMissing) {
    // Simulate the narrow force-kill window: install->backup succeeded but
    // staging->install (and its own compensating restore, on failure) never
    // ran, leaving install_dir gone and backup_dir holding the old version --
    // the exact state SwapError::RestoreFailed reports.
    fs::rename(install, fs::path(plan.backup_dir));
    ASSERT_FALSE(fs::exists(install));
    ASSERT_TRUE(fs::exists(plan.backup_dir));

    EXPECT_TRUE(RepairOrphanedSwap(plan));

    std::ifstream live(install / "exosnap.exe");
    std::string s((std::istreambuf_iterator<char>(live)), {});
    EXPECT_EQ(s, "old");
    EXPECT_FALSE(fs::exists(plan.backup_dir));
}

TEST(SwapEngine, ReadFileVersionOnSystemDllAndGarbage) {
    EXPECT_TRUE(ReadFileVersion(L"C:\\Windows\\System32\\kernel32.dll").has_value());
    auto txt = fs::temp_directory_path() / "exosnap_notanexe.txt";
    std::ofstream(txt) << "hi";
    EXPECT_FALSE(ReadFileVersion(txt.wstring()).has_value());
    fs::remove(txt);
}

TEST(SwapEngine, ReadProductVersionStringOnSystemDllAndGarbage) {
    // kernel32 always carries a StringFileInfo ProductVersion.
    EXPECT_TRUE(ReadProductVersionString(L"C:\\Windows\\System32\\kernel32.dll").has_value());
    auto txt = fs::temp_directory_path() / "exosnap_notanexe2.txt";
    std::ofstream(txt) << "hi";
    EXPECT_FALSE(ReadProductVersionString(txt.wstring()).has_value());
    fs::remove(txt);
}

// The installed-version check must be prerelease-aware: the numeric
// FIXEDFILEINFO of an rc build reads 0.9.0, and only the ProductVersion string
// can prove the rc identity. A prerelease target verified against a
// base-only product string (or vice versa) must fail.
TEST(SwapEngine, InstalledVersionMatchesPrefersProductVersionString) {
    const SemVer rc4 = *ParseSemVer("0.9.0-rc4");
    const SemVer rc3 = *ParseSemVer("0.9.0-rc3");
    const SemVer base = *ParseSemVer("0.9.0");

    // Product string present: authoritative, prerelease-aware.
    EXPECT_TRUE(InstalledVersionMatches(base, rc4, rc4));
    EXPECT_FALSE(InstalledVersionMatches(base, rc3, rc4));
    EXPECT_FALSE(InstalledVersionMatches(base, base, rc4));
    EXPECT_FALSE(InstalledVersionMatches(base, rc4, base));
    EXPECT_TRUE(InstalledVersionMatches(base, base, base));

    // No product string: an RC must fail closed because the numeric base cannot
    // distinguish rc4 from the final. A final target keeps the legacy fallback.
    EXPECT_FALSE(InstalledVersionMatches(base, std::nullopt, rc4));
    EXPECT_TRUE(InstalledVersionMatches(base, std::nullopt, base));
    EXPECT_FALSE(InstalledVersionMatches(*ParseSemVer("0.8.1"), std::nullopt, rc4));

    // Nothing readable at all: never verified.
    EXPECT_FALSE(InstalledVersionMatches(std::nullopt, std::nullopt, rc4));
    EXPECT_FALSE(InstalledVersionMatches(std::nullopt, std::nullopt, base));
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

// The updater's close/handoff message must reach exactly the app's real main
// window: never merely a same-titled window from a different process (a
// second already-running instance can carry the same title, leaving the real
// target waiting out its close timeout untouched), and never merely some
// other window the SAME process happens to own (ExoSnap's system tray icon
// gives Qt a hidden native callback window under the same PID; matching on
// PID alone can silently grab that one instead, and the message is then
// swallowed because only the real main window's nativeEvent reacts to it).
// Matching requires both owner PID and exact title.
TEST(SwapEngine, SelectWindowForProcessRequiresBothPidAndTitle) {
    const std::vector<TopLevelWindow> candidates = {
        {reinterpret_cast<void*>(0x1), 111, L"ExoSnap"},          // wrong pid
        {reinterpret_cast<void*>(0x2), 222, L""},                 // right pid, hidden helper window (no title)
        {reinterpret_cast<void*>(0x3), 222, L"ExoSnap"},          // right pid, right title
        {reinterpret_cast<void*>(0x4), 222, L"ExoSnap - Second"}, // right pid, wrong title
    };
    EXPECT_EQ(SelectWindowForProcess(candidates, 222, L"ExoSnap"), reinterpret_cast<void*>(0x3));
    EXPECT_EQ(SelectWindowForProcess(candidates, 333, L"ExoSnap"), nullptr);
    EXPECT_EQ(SelectWindowForProcess({}, 222, L"ExoSnap"), nullptr);
}

TEST(SwapEngine, FindTopLevelWindowForProcessFindsRealWindowByPidAndTitle) {
    EXPECT_EQ(FindTopLevelWindowForProcess(0, L"ExoSnap"), nullptr); // pid 0 owns no window

    const wchar_t* kClassName = L"ExoSnapTestSwapEngineWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    const ATOM registered = ::RegisterClassW(&wc);
    ASSERT_NE(registered, 0);

    // Untitled window created FIRST, standing in for Qt's hidden system-tray
    // callback window: same process, same class, enumerated before the real
    // main window. If matching only checked PID, this one would win.
    HWND hidden = ::CreateWindowExW(0, kClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, nullptr, nullptr,
                                    wc.hInstance, nullptr);
    ASSERT_NE(hidden, nullptr);

    // Same title a colliding second instance could plausibly also use --
    // proving the match still lands on this process's own window, not on
    // title alone.
    HWND window = ::CreateWindowExW(0, kClassName, L"ExoSnap", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                    wc.hInstance, nullptr);
    ASSERT_NE(window, nullptr);

    const DWORD self_pid = ::GetCurrentProcessId();
    EXPECT_EQ(FindTopLevelWindowForProcess(self_pid, L"ExoSnap"), reinterpret_cast<void*>(window));

    ::DestroyWindow(window);
    ::DestroyWindow(hidden);
    ::UnregisterClassW(kClassName, wc.hInstance);
    EXPECT_EQ(FindTopLevelWindowForProcess(self_pid, L"ExoSnap"), nullptr);
}
