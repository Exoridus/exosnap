#include <gtest/gtest.h>

#include "../src/install_mode_classify.h"

using exosnap::update::ClassifyInstallMode;
using exosnap::update::InstallMode;
using exosnap::update::NormalizeDirForCompare;

TEST(NormalizeDirForCompare, IgnoresCaseTrailingSeparatorAndSlashDirection) {
    EXPECT_EQ(NormalizeDirForCompare(L"C:\\Program Files\\Codexo\\ExoSnap\\"),
              NormalizeDirForCompare(L"c:/program files/codexo/exosnap"));
}

TEST(ClassifyInstallMode, NoMarkerIsPortable) {
    EXPECT_EQ(ClassifyInstallMode(false, std::nullopt, L"C:\\rc\\rc15"), InstallMode::Portable);
    // Even standing in the install directory: without the marker nothing installed it.
    EXPECT_EQ(ClassifyInstallMode(false, L"C:\\Program Files\\Codexo\\ExoSnap", L"C:\\Program Files\\Codexo\\ExoSnap"),
              InstallMode::Portable);
}

TEST(ClassifyInstallMode, MarkerAndMatchingDirectoryIsInstalled) {
    EXPECT_EQ(ClassifyInstallMode(true, L"C:\\Program Files\\Codexo\\ExoSnap", L"C:\\Program Files\\Codexo\\ExoSnap"),
              InstallMode::Installed);
    EXPECT_EQ(ClassifyInstallMode(true, L"C:\\Program Files\\Codexo\\ExoSnap\\", L"c:\\program files\\codexo\\exosnap"),
              InstallMode::Installed);
}

// The defect this function exists for: a portable copy on a machine that also
// carries an MSI install used to inherit the marker, claim installMode
// "installed" with its own directory, and be refused by the updater as a
// registry mismatch -- so it could never update itself.
TEST(ClassifyInstallMode, MarkerButDifferentDirectoryIsPortable) {
    EXPECT_EQ(ClassifyInstallMode(true, L"C:\\Program Files\\Codexo\\ExoSnap",
                                  L"C:\\rc\\rc15\\ExoSnap-0.9.0-rc15-windows-x64-portable"),
              InstallMode::Portable);
}

TEST(ClassifyInstallMode, MarkerWithoutRegistryPathStaysInstalled) {
    EXPECT_EQ(ClassifyInstallMode(true, std::nullopt, L"C:\\anywhere"), InstallMode::Installed);
    EXPECT_EQ(ClassifyInstallMode(true, std::wstring{}, L"C:\\anywhere"), InstallMode::Installed);
}

TEST(ClassifyInstallMode, UnknownOwnDirectoryFallsBackToTheMarker) {
    EXPECT_EQ(ClassifyInstallMode(true, L"C:\\Program Files\\Codexo\\ExoSnap", L""), InstallMode::Installed);
}
