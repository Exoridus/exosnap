// Unit tests for the pure planning pieces behind UpdaterWorker plus a
// structural smoke of the worker itself.
//
// No QApplication and no network: ResolveStagedRoot is exercised on throwaway
// temp directories, RetryEntryStep / BuildMsiexecParams are pure values, and
// the worker smoke drives a canned failure (a base URL that is rejected before
// any WinHTTP call) through direct signal connections on the test thread.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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
