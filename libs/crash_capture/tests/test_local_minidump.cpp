// test_local_minidump.cpp — Unit tests for the in-process minidump fallback.
//
// The dump-writing path itself cannot be unit-tested (it runs from an exception
// filter, in a dying process). What is pinned here is the pure surface: when the
// fallback installs, and how a dump is named.

#include <crash_capture/local_minidump.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace exosnap::crash_capture;

// ---------------------------------------------------------------------------
// ShouldInstallLocalMinidumpHandler — Crashpad owns the filter when present.
// ---------------------------------------------------------------------------
TEST(LocalMinidump, InstallsWhenNoCrashpad) {
    EXPECT_TRUE(ShouldInstallLocalMinidumpHandler(/*crashpad_active=*/false));
}

TEST(LocalMinidump, DefersToCrashpadWhenActive) {
    EXPECT_FALSE(ShouldInstallLocalMinidumpHandler(/*crashpad_active=*/true));
}

// ---------------------------------------------------------------------------
// MakeMinidumpFileName — chronological, zero-padded, pid-unique.
// ---------------------------------------------------------------------------
TEST(LocalMinidump, FileNameHasStableShape) {
    EXPECT_EQ(MakeMinidumpFileName(2026, 7, 10, 14, 30, 12, 1234), "exosnap-20260710-143012-1234.dmp");
}

TEST(LocalMinidump, FileNameZeroPadsEveryField) {
    EXPECT_EQ(MakeMinidumpFileName(2026, 1, 2, 3, 4, 5, 7), "exosnap-20260102-030405-7.dmp");
}

TEST(LocalMinidump, FileNamesSortChronologically) {
    const std::string earlier = MakeMinidumpFileName(2026, 7, 10, 9, 59, 59, 1);
    const std::string later = MakeMinidumpFileName(2026, 7, 10, 10, 0, 0, 1);
    EXPECT_LT(earlier, later);
}

TEST(LocalMinidump, FileNamesDifferPerProcess) {
    const std::string a = MakeMinidumpFileName(2026, 7, 10, 14, 30, 12, 1234);
    const std::string b = MakeMinidumpFileName(2026, 7, 10, 14, 30, 12, 5678);
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// InstallLocalMinidumpHandler — argument validation only; the filter itself is
// left installed (harmless: it only fires on an unhandled exception).
// ---------------------------------------------------------------------------
TEST(LocalMinidump, RejectsEmptyCrashDir) {
    EXPECT_FALSE(InstallLocalMinidumpHandler(""));
}

TEST(LocalMinidump, AcceptsExistingDirectory) {
    const fs::path dir = fs::temp_directory_path() / "exosnap_local_minidump_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    EXPECT_TRUE(InstallLocalMinidumpHandler(dir.string()));
    fs::remove_all(dir, ec);
}

TEST(LocalMinidump, IsIdempotent) {
    const fs::path dir = fs::temp_directory_path() / "exosnap_local_minidump_test2";
    std::error_code ec;
    fs::create_directories(dir, ec);
    EXPECT_TRUE(InstallLocalMinidumpHandler(dir.string()));
    EXPECT_TRUE(InstallLocalMinidumpHandler(dir.string()));
    fs::remove_all(dir, ec);
}
