#include <gtest/gtest.h>

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
