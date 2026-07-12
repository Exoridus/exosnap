// Unit tests for the pure planning pieces behind UpdaterWorker plus a
// structural smoke of the worker itself.
//
// No QApplication and no network: ResolveStagedRoot is exercised on throwaway
// temp directories, RetryEntryStep / BuildMsiexecParams are pure values, and
// the worker smoke drives a canned failure (a base URL that is rejected before
// any WinHTTP call) through direct signal connections on the test thread.

#include <gtest/gtest.h>

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format on

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <update/package_verifier.h>

#include "UpdaterWorker.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// ResolveStagedRoot -- temp-dir fixture
// ---------------------------------------------------------------------------

class ResolveStagedRootTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                (L"exosnap_staged_root_test_" +
                 std::to_wstring(::testing::UnitTest::GetInstance()->random_seed()) + L"_" +
                 std::to_wstring(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void Touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << "x";
    }

    fs::path root_;
};

TEST_F(ResolveStagedRootTest, FlatLayoutWithExeStays) {
    Touch(root_ / "exosnap.exe");
    Touch(root_ / "Qt6Core.dll");

    const std::optional<std::wstring> resolved = ResolveStagedRoot(root_.wstring());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(fs::path(*resolved), root_);
}

TEST_F(ResolveStagedRootTest, SingleNestedDirDescendsOneLevel) {
    const fs::path inner = root_ / "ExoSnap-0.9.0-windows-x64-portable";
    Touch(inner / "exosnap.exe");

    const std::optional<std::wstring> resolved = ResolveStagedRoot(root_.wstring());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(fs::path(*resolved), inner);
}

TEST_F(ResolveStagedRootTest, SingleNestedDirWithoutExeFails) {
    // A lone top-level folder that carries no exosnap.exe is an unusable
    // package, not a layout to descend into.
    Touch(root_ / "ExoSnap-0.9.0-windows-x64-portable" / "Qt6Core.dll");
    EXPECT_FALSE(ResolveStagedRoot(root_.wstring()).has_value());
}

TEST_F(ResolveStagedRootTest, EmptyDirFails) {
    EXPECT_FALSE(ResolveStagedRoot(root_.wstring()).has_value());
}

TEST_F(ResolveStagedRootTest, MissingDirFails) {
    EXPECT_FALSE(ResolveStagedRoot((root_ / L"does_not_exist").wstring()).has_value());
}

TEST_F(ResolveStagedRootTest, TwoDirsWithoutExeFails) {
    Touch(root_ / "a" / "exosnap.exe");
    Touch(root_ / "b" / "exosnap.exe");
    EXPECT_FALSE(ResolveStagedRoot(root_.wstring()).has_value());
}

TEST_F(ResolveStagedRootTest, StrayFileBesideSingleDirFails) {
    // A file next to the single top-level dir means descending would silently
    // drop content -- refuse instead of guessing.
    Touch(root_ / "README.txt");
    Touch(root_ / "inner" / "exosnap.exe");
    EXPECT_FALSE(ResolveStagedRoot(root_.wstring()).has_value());
}

TEST_F(ResolveStagedRootTest, TopLevelExeWinsOverSingleDir) {
    // Flat layout that also happens to carry a subdirectory (plugins/) must
    // NOT descend: the exe marks the root.
    Touch(root_ / "exosnap.exe");
    Touch(root_ / "plugins" / "platforms" / "qwindows.dll");

    const std::optional<std::wstring> resolved = ResolveStagedRoot(root_.wstring());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(fs::path(*resolved), root_);
}

// ---------------------------------------------------------------------------
// RetryEntryStep -- FailureCase -> pipeline re-entry mapping
// ---------------------------------------------------------------------------

TEST(RetryEntryStep, DownloadFailuresReenterDownload) {
    EXPECT_EQ(RetryEntryStep(FailureCase::DownloadFailed), UpStep::Download);       // A1
    EXPECT_EQ(RetryEntryStep(FailureCase::VerifyDownloadFailed), UpStep::Download); // A2
}

TEST(RetryEntryStep, AppWontCloseReentersCloseApp) {
    EXPECT_EQ(RetryEntryStep(FailureCase::AppWontClose), UpStep::CloseApp); // B1
}

TEST(RetryEntryStep, InstallFailuresReenterInstall) {
    EXPECT_EQ(RetryEntryStep(FailureCase::InstallFailed), UpStep::Install);       // B2
    EXPECT_EQ(RetryEntryStep(FailureCase::VerifyInstallFailed), UpStep::Install); // B3
    EXPECT_EQ(RetryEntryStep(FailureCase::UacDeclined), UpStep::Install);         // C1 re-handoff
    EXPECT_EQ(RetryEntryStep(FailureCase::MsiFailed), UpStep::Install);           // C2
}

TEST(RetryEntryStep, LaunchFailedReentersLaunch) {
    EXPECT_EQ(RetryEntryStep(FailureCase::LaunchFailed), UpStep::Launch); // B4
}

// ---------------------------------------------------------------------------
// BuildMsiexecParams -- silent-install command line (quoting)
// ---------------------------------------------------------------------------

TEST(BuildMsiexecParams, QuotesPathAndAppendsSilentFlags) {
    EXPECT_EQ(BuildMsiexecParams(L"C:\\Temp\\pkg.msi"), L"/i \"C:\\Temp\\pkg.msi\" /qn /norestart");
}

TEST(BuildMsiexecParams, PathWithSpacesStaysOneArgument) {
    EXPECT_EQ(BuildMsiexecParams(L"C:\\Users\\Some User\\AppData\\Local\\Temp\\ExoSnapUpdate\\0.9.0\\package.msi"),
              L"/i \"C:\\Users\\Some User\\AppData\\Local\\Temp\\ExoSnapUpdate\\0.9.0\\package.msi\" /qn /norestart");
}

// ---------------------------------------------------------------------------
// WaitForProcessOrCancel -- bounded, cancel-aware replacement for the MSI
// handoff's old WaitForSingleObject(..., INFINITE). Exercised against a
// manual-reset event rather than a real msiexec process: WaitForSingleObject
// accepts any waitable kernel object, and an event gives full, instant control
// over "signaled" without spawning anything.
// ---------------------------------------------------------------------------

class WaitForProcessOrCancelTest : public ::testing::Test {
  protected:
    void SetUp() override {
        event_ = ::CreateEventW(nullptr, /*bManualReset=*/TRUE, /*bInitialState=*/FALSE, nullptr);
        ASSERT_NE(event_, nullptr);
    }
    void TearDown() override {
        if (event_ != nullptr) {
            ::CloseHandle(event_);
        }
    }
    HANDLE event_ = nullptr;
};

TEST_F(WaitForProcessOrCancelTest, ReturnsTrueWhenAlreadySignaled) {
    ::SetEvent(event_);
    std::atomic<bool> cancel{false};
    EXPECT_TRUE(WaitForProcessOrCancel(event_, std::chrono::seconds(5), cancel));
}

TEST_F(WaitForProcessOrCancelTest, ReturnsFalsePromptlyWhenCancelIsAlreadySet) {
    // Never signaled; cancel is set before the call. The wait must give up on
    // (at most) the first poll tick, not the full timeout.
    std::atomic<bool> cancel{true};
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(WaitForProcessOrCancel(event_, std::chrono::seconds(30), cancel));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(1000));
}

TEST_F(WaitForProcessOrCancelTest, ReturnsFalseWhenTimeoutElapsesUnsignaled) {
    std::atomic<bool> cancel{false};
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(WaitForProcessOrCancel(event_, std::chrono::milliseconds(300), cancel));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, std::chrono::milliseconds(300));
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

TEST_F(WaitForProcessOrCancelTest, DoesNotFalsePositiveBeforeSignaledOrCancelled) {
    // A short timeout with neither signal nor cancel must still report false
    // rather than true -- guards against an inverted return value.
    std::atomic<bool> cancel{false};
    EXPECT_FALSE(WaitForProcessOrCancel(event_, std::chrono::milliseconds(150), cancel));
}

// ---------------------------------------------------------------------------
// OpenPackageWriteLock + VerifyPackageHandle -- the TOCTOU close: the verified
// package cannot be written, renamed, or deleted while the lock is held, so the
// bytes that hash here are the bytes later consumed (elevated msiexec).
// ---------------------------------------------------------------------------

class PackageLockTest : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = fs::temp_directory_path() /
                (L"exosnap_pkg_lock_test_" + std::to_wstring(reinterpret_cast<uintptr_t>(this)) + L".bin");
        fs::remove(path_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(path_, ec);
    }

    void Write(const std::string& bytes) {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // SHA-256("abc"), the canonical NIST test vector.
    static constexpr const char* kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    fs::path path_;
};

TEST_F(PackageLockTest, LockOpensExistingFileAndVerifiesThroughHandle) {
    Write("abc");
    void* lock = OpenPackageWriteLock(path_.wstring());
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    EXPECT_EQ(exosnap::update::VerifyPackageHandle(lock, kAbcSha256), exosnap::update::VerifyResult::Ok);
    ClosePackageLock(lock);
}

TEST_F(PackageLockTest, HeldLockDeniesConcurrentWriteOpen) {
    // The core of the fix: while the deny-write lock is held, no same-user
    // process can open the verified file for writing to swap its contents.
    Write("abc");
    void* lock = OpenPackageWriteLock(path_.wstring());
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    HANDLE writer = ::CreateFileW(path_.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    if (writer != INVALID_HANDLE_VALUE) {
        ::CloseHandle(writer);
    }
    ClosePackageLock(lock);
}

TEST_F(PackageLockTest, HeldLockDeniesDeleteAndRename) {
    // deny-delete (no FILE_SHARE_DELETE) also blocks rename, which pins the path
    // — and, via the open child handle, its parent directories — so the absolute
    // path handed to msiexec cannot be repointed at a different file.
    Write("abc");
    void* lock = OpenPackageWriteLock(path_.wstring());
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    EXPECT_EQ(::DeleteFileW(path_.wstring().c_str()), 0);
    const fs::path other = path_.wstring() + L".swapped";
    EXPECT_EQ(::MoveFileW(path_.wstring().c_str(), other.wstring().c_str()), 0);
    EXPECT_TRUE(fs::exists(path_));

    ClosePackageLock(lock);
}

TEST_F(PackageLockTest, ReleasingLockRestoresWriteAccess) {
    // Sanity: without the lock the file IS writable/replaceable — this is exactly
    // the pre-fix window the held lock removes.
    Write("abc");
    void* lock = OpenPackageWriteLock(path_.wstring());
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    ClosePackageLock(lock);

    HANDLE writer = ::CreateFileW(path_.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(writer, INVALID_HANDLE_VALUE);
    if (writer != INVALID_HANDLE_VALUE) {
        ::CloseHandle(writer);
    }
}

TEST_F(PackageLockTest, VerifyThroughHandleRejectsWrongHash) {
    Write("abc");
    void* lock = OpenPackageWriteLock(path_.wstring());
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    EXPECT_EQ(exosnap::update::VerifyPackageHandle(lock, std::string(64, '0')),
              exosnap::update::VerifyResult::PackageHashMismatch);
    ClosePackageLock(lock); // handle-based verify never deletes; caller owns the file
    EXPECT_TRUE(fs::exists(path_));
}

TEST_F(PackageLockTest, VerifyThroughHandleRewindsBeforeHashing) {
    // Hashing must cover the whole file regardless of the handle's file pointer.
    Write("abc");
    void* lock = OpenPackageWriteLock(path_.wstring());
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    LARGE_INTEGER end{};
    end.QuadPart = 3;
    ::SetFilePointerEx(static_cast<HANDLE>(lock), end, nullptr, FILE_BEGIN); // move to EOF
    EXPECT_EQ(exosnap::update::VerifyPackageHandle(lock, kAbcSha256), exosnap::update::VerifyResult::Ok);
    ClosePackageLock(lock);
}

TEST(VerifyPackageHandle, NullOrInvalidHandleIsPackageNotFound) {
    EXPECT_EQ(exosnap::update::VerifyPackageHandle(nullptr, std::string(64, '0')),
              exosnap::update::VerifyResult::PackageNotFound);
    EXPECT_EQ(exosnap::update::VerifyPackageHandle(INVALID_HANDLE_VALUE, std::string(64, '0')),
              exosnap::update::VerifyResult::PackageNotFound);
}

// ---------------------------------------------------------------------------
// UpdaterWorker structural smoke -- construct, wire signals, drive a canned
// failure. The base URL is rejected by FetchReleasesJson before any network
// I/O, so this exercises the emission contract only.
// ---------------------------------------------------------------------------

TEST(UpdaterWorkerSmoke, InvalidBaseUrlEmitsDownloadFailed) {
    UpdaterArgs args;
    args.channel = exosnap::update::UpdateChannel::Stable;
    args.install_mode = exosnap::update::InstallMode::Portable;
    args.install_dir = QStringLiteral("C:\\nonexistent\\exosnap-install");
    args.app_pid = 0;
    args.current_version = QStringLiteral("0.8.1");
    args.base_url = QStringLiteral("http://127.0.0.1/releases"); // not https -> rejected pre-network

    UpdaterWorker worker(args);

    std::vector<UpStep> started;
    std::vector<UpStep> done;
    std::optional<FailureCase> failure;
    QString failure_detail;
    bool all_done = false;

    QObject::connect(&worker, &UpdaterWorker::stepStarted, [&](UpStep s) { started.push_back(s); });
    QObject::connect(&worker, &UpdaterWorker::stepDone, [&](UpStep s) { done.push_back(s); });
    QObject::connect(&worker, &UpdaterWorker::allDone, [&] { all_done = true; });
    QObject::connect(&worker, &UpdaterWorker::failed, [&](FailureCase c, const QString& detail) {
        failure = c;
        failure_detail = detail;
    });

    worker.run(UpStep::Download);

    ASSERT_EQ(started.size(), 1u);
    EXPECT_EQ(started.front(), UpStep::Download);
    EXPECT_TRUE(done.empty());
    EXPECT_FALSE(all_done);
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(*failure, FailureCase::DownloadFailed); // A1 amber path
    EXPECT_FALSE(failure_detail.isEmpty());
}

} // namespace
