// test_update_launch_plan.cpp -- pure helpers behind UpdateService::LaunchUpdater.
//
// These cover the UI-agnostic staging/argument/guard logic so the actual
// CreateProcess + file-copy path in LaunchUpdater() stays a thin shell:
//   * UpdaterStagingFileList()  -- the files copied into the staged updater dir.
//   * BuildUpdaterArgs()        -- the argv the app hands the staged updater,
//                                  round-tripped through the updater's own parser.
//   * UpdateService::IsScoopManagedInstall() -- notify-only Scoop detection.

#include <gtest/gtest.h>

#include <QStringList>

#include "../apps/updater/UpdaterArgs.h"
#include "services/UpdateService.h"
#include "services/VerifyReinstallMode.h"

namespace {

using exosnap::UpdateService;
namespace upd = exosnap::update;

// -- UpdaterStagingFileList -------------------------------------------------

TEST(UpdaterStagingFileList, ContainsFourMandatoryEntries) {
    const QStringList list = exosnap::UpdaterStagingFileList();
    EXPECT_TRUE(list.contains(QStringLiteral("exosnap-updater.exe")))
        << "staging list must include the updater executable";
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Core.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Gui.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Widgets.dll")));
}

TEST(UpdaterStagingFileList, IncludesPlatformPlugin) {
    const QStringList list = exosnap::UpdaterStagingFileList();
    // The Qt Widgets updater needs the windows platform plugin to show a window.
    bool has_platform = false;
    for (const QString& e : list) {
        if (e.contains(QStringLiteral("qwindows.dll")))
            has_platform = true;
    }
    EXPECT_TRUE(has_platform) << "staging list must include plugins/platforms/qwindows.dll";
}

// -- BuildUpdaterArgs round-trip -------------------------------------------

TEST(BuildUpdaterArgs, RoundTripsInstalled) {
    upd::UpdateState st;
    st.channel = upd::UpdateChannel::Preview;
    st.install_mode = upd::InstallMode::Installed;

    QStringList argv;
    argv << QStringLiteral("exosnap-updater.exe");
    argv += exosnap::BuildUpdaterArgs(st, QStringLiteral("C:/Program Files/Codexo/ExoSnap"), 4242u,
                                      QStringLiteral("0.9.0"));

    const auto parsed = ParseUpdaterArgs(argv);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->channel, upd::UpdateChannel::Preview);
    EXPECT_EQ(parsed->install_mode, upd::InstallMode::Installed);
    EXPECT_EQ(parsed->install_dir, QStringLiteral("C:/Program Files/Codexo/ExoSnap"));
    EXPECT_EQ(parsed->app_pid, 4242u);
    EXPECT_EQ(parsed->current_version, QStringLiteral("0.9.0"));
}

TEST(BuildUpdaterArgs, RoundTripsPortable) {
    upd::UpdateState st;
    st.channel = upd::UpdateChannel::Stable;
    st.install_mode = upd::InstallMode::Portable;

    QStringList argv;
    argv << QStringLiteral("exosnap-updater.exe");
    argv += exosnap::BuildUpdaterArgs(st, QStringLiteral("D:/Tools/ExoSnap"), 7u, QStringLiteral("1.2.3"));

    const auto parsed = ParseUpdaterArgs(argv);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->channel, upd::UpdateChannel::Stable);
    EXPECT_EQ(parsed->install_mode, upd::InstallMode::Portable);
    EXPECT_EQ(parsed->install_dir, QStringLiteral("D:/Tools/ExoSnap"));
    EXPECT_EQ(parsed->app_pid, 7u);
    EXPECT_EQ(parsed->current_version, QStringLiteral("1.2.3"));
}

// A normal update run must never hand the updater the verification gate.
TEST(BuildUpdaterArgs, OmitsVerifyReinstallByDefault) {
    upd::UpdateState st;
    st.install_mode = upd::InstallMode::Installed;
    const QStringList flags = exosnap::BuildUpdaterArgs(st, QStringLiteral("C:/x"), 1u, QStringLiteral("0.9.0"));
    EXPECT_FALSE(flags.contains(QStringLiteral("--verify-reinstall")));
}

TEST(BuildUpdaterArgs, VerifyReinstallRoundTripsAsABooleanFlag) {
    upd::UpdateState st;
    st.channel = upd::UpdateChannel::Preview;
    st.install_mode = upd::InstallMode::Portable;

    QStringList argv;
    argv << QStringLiteral("exosnap-updater.exe");
    argv += exosnap::BuildUpdaterArgs(st, QStringLiteral("D:/Tools/ExoSnap"), 11u, QStringLiteral("0.9.0-rc4"),
                                      /*verify_reinstall=*/true);

    const auto parsed = ParseUpdaterArgs(argv);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->verify_reinstall);
    EXPECT_EQ(parsed->current_version, QStringLiteral("0.9.0-rc4"));
    EXPECT_EQ(parsed->install_dir, QStringLiteral("D:/Tools/ExoSnap")) << "the boolean flag must not eat a value";
}

// -- HasVerifyUpdateReinstallRequest ----------------------------------------

TEST(VerifyUpdateReinstallFlag, AbsentByDefault) {
    EXPECT_FALSE(exosnap::services::HasVerifyUpdateReinstallRequest(
        QStringList{QStringLiteral("exosnap.exe"), QStringLiteral("--relaunch-page"), QStringLiteral("Settings")}));
}

TEST(VerifyUpdateReinstallFlag, RecognisedAnywhereInArgv) {
    EXPECT_TRUE(exosnap::services::HasVerifyUpdateReinstallRequest(
        QStringList{QStringLiteral("exosnap.exe"), QStringLiteral("--verify-update-reinstall")}));
    EXPECT_TRUE(exosnap::services::HasVerifyUpdateReinstallRequest(QStringList{
        QStringLiteral("exosnap.exe"), QStringLiteral("--verify-update-reinstall"), QStringLiteral("--other")}));
}

// A longer or differently-spelled flag must not switch the mode on.
TEST(VerifyUpdateReinstallFlag, RequiresAnExactMatch) {
    EXPECT_FALSE(exosnap::services::HasVerifyUpdateReinstallRequest(
        QStringList{QStringLiteral("exosnap.exe"), QStringLiteral("--verify-update-reinstall-now")}));
    EXPECT_FALSE(exosnap::services::HasVerifyUpdateReinstallRequest(
        QStringList{QStringLiteral("exosnap.exe"), QStringLiteral("--verify-update-reinstall=1")}));
}

// -- IsScoopManagedInstall --------------------------------------------------

TEST(IsScoopManagedInstall, TrueForScoopPath) {
    EXPECT_TRUE(UpdateService::IsScoopManagedInstall(QStringLiteral("C:/Users/x/scoop/apps/exosnap/current")));
}

TEST(IsScoopManagedInstall, TrueForBackslashAndMixedCase) {
    EXPECT_TRUE(UpdateService::IsScoopManagedInstall(QStringLiteral("C:\\Users\\x\\Scoop\\Apps\\exosnap\\current")));
}

TEST(IsScoopManagedInstall, FalseForProgramFiles) {
    EXPECT_FALSE(UpdateService::IsScoopManagedInstall(QStringLiteral("C:/Program Files/Codexo/ExoSnap")));
}

TEST(IsScoopManagedInstall, FalseForPortableToolsDir) {
    EXPECT_FALSE(UpdateService::IsScoopManagedInstall(QStringLiteral("D:/Tools/ExoSnap")));
}

// Relocated Scoop root ($env:SCOOP): "<root>/apps/<name>/current" carries no
// literal "scoop" segment but still uses the apps/current junction layout.
TEST(IsScoopManagedInstall, TrueForRelocatedRootWithAppsAndCurrent) {
    EXPECT_TRUE(UpdateService::IsScoopManagedInstall(QStringLiteral("C:/tools/myscoop/apps/exosnap/current")));
}

TEST(IsScoopManagedInstall, FalseForProgramFilesNoAppsNoCurrent) {
    EXPECT_FALSE(UpdateService::IsScoopManagedInstall(QStringLiteral("C:/Program Files/Codexo/ExoSnap")));
}

// An "/apps/" segment alone (no "current" component) must not match — that's a
// generic portable layout, not Scoop's junction tree.
TEST(IsScoopManagedInstall, FalseForAppsDirWithoutCurrent) {
    EXPECT_FALSE(UpdateService::IsScoopManagedInstall(QStringLiteral("D:/apps/ExoSnap")));
}

// "apps" and "current" both present but not in Scoop's "apps/<name>/current"
// adjacency (current isn't exactly two components after apps) must not match.
TEST(IsScoopManagedInstall, FalseForAppsAndCurrentWrongAdjacency) {
    EXPECT_FALSE(UpdateService::IsScoopManagedInstall(QStringLiteral("C:/apps/current/ExoSnap")));
}

TEST(IsScoopManagedInstall, FalseForCurrentBeforeApps) {
    EXPECT_FALSE(UpdateService::IsScoopManagedInstall(QStringLiteral("D:/Media/current/apps/")));
}

// -- ResolveUpdateCardState (loop guard + stuck-pending recovery) -----------

TEST(ResolveUpdateCardState, UpToDateWhenNoUpdate) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/false, /*is_scoop=*/false, QString(),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("uptodate"));
}

TEST(ResolveUpdateCardState, ScoopWinsOverAvailable) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/true, QString(),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("scoop"));
}

TEST(ResolveUpdateCardState, AvailableWhenNoStamp) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QString(),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("available"));
}

// Loop guard (automatic startup check): the updater already launched for this
// version (applied_version stamp == available), so the card stays "pending".
TEST(ResolveUpdateCardState, PendingWhenStampMatchesAvailable) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QStringLiteral("2.0.0"),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("pending"));
}

// A newer version than the stamped one is offered normally.
TEST(ResolveUpdateCardState, AvailableWhenStampIsOlderVersion) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QStringLiteral("2.0.0"),
                                              QStringLiteral("2.1.0")),
              QStringLiteral("available"));
}

// Recovery: a stuck "Restart pending" (updater launched but never swapped) is
// cleared by a manual check clearing applied_version BEFORE the check runs. With
// an empty stamp, the same still-applicable version re-arms to "available".
TEST(ResolveUpdateCardState, RearmsToAvailableAfterManualCheckClearsStamp) {
    // Automatic re-check with the stamp still set -> pending.
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QStringLiteral("2.0.0"),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("pending"));
    // Manual check clears the stamp upstream; resolver now sees an empty stamp.
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QString(),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("available"));
}

// -- ResolveUpdateCardState: verification reinstall (ADR 0055) --------------

TEST(ResolveUpdateCardState, VerifyReinstallWhenModeIsOnAndTheOfferIsTheRunningVersion) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QString(),
                                              QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("verify-reinstall"));
}

// The mode does not turn every offer into a reinstall: a genuinely newer release
// is still a normal update.
TEST(ResolveUpdateCardState, AvailableWhenVerifyModeIsOnButTheOfferIsNewer) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QString(),
                                              QStringLiteral("0.9.0"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("available"));
}

// Without the mode, an offer equal to the running version cannot reach the
// reinstall state at all (the engine would not offer it in the first place).
TEST(ResolveUpdateCardState, NoVerifyReinstallWhenTheModeIsOff) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QString(),
                                              QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/false,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("available"));
}

// Scoop trees are never touched by the staged swap — not even in verify mode.
TEST(ResolveUpdateCardState, ScoopStillWinsInVerifyMode) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/true, QString(),
                                              QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("scoop"));
}

TEST(ResolveUpdateCardState, UpToDateStillWinsInVerifyMode) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/false, /*is_scoop=*/false, QString(),
                                              QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("uptodate"));
}

// The loop guard exists to stop a stale cache from re-offering an update that is
// already staged. Re-running the swap for the SAME version is exactly what
// verification mode is for, so it outranks the guard.
TEST(ResolveUpdateCardState, VerifyReinstallOutranksThePendingLoopGuard) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false,
                                              QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc4"),
                                              /*verify_reinstall_mode=*/true, QStringLiteral("0.9.0-rc4")),
              QStringLiteral("verify-reinstall"));
}

TEST(ResolveUpdateCardState, VerifyModeWithoutAnOfferedVersionFallsBack) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QString(), QString(),
                                              /*verify_reinstall_mode=*/true, QString()),
              QStringLiteral("available"));
}

} // namespace
