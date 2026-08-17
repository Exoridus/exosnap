// Unit tests for the updater command-line parser.

#include <gtest/gtest.h>

#include "UpdaterArgs.h"

namespace {

using exosnap::update::InstallMode;
using exosnap::update::UpdateChannel;

QStringList FullArgLine() {
    return QStringList{
        QStringLiteral("exosnap-updater.exe"),   QStringLiteral("--channel"),       QStringLiteral("preview"),
        QStringLiteral("--install-mode"),        QStringLiteral("portable"),        QStringLiteral("--install-dir"),
        QStringLiteral("C:/Apps/ExoSnap"),       QStringLiteral("--app-pid"),       QStringLiteral("4321"),
        QStringLiteral("--current-version"),     QStringLiteral("0.8.1"),           QStringLiteral("--base-url"),
        QStringLiteral("http://localhost:8080"), QStringLiteral("--preview-state"), QStringLiteral("amber"),
    };
}

TEST(UpdaterArgs, FullArgLineParses) {
    auto args = ParseUpdaterArgs(FullArgLine());
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(args->channel, UpdateChannel::Preview);
    EXPECT_EQ(args->install_mode, InstallMode::Portable);
    EXPECT_EQ(args->install_dir, QStringLiteral("C:/Apps/ExoSnap"));
    EXPECT_EQ(args->app_pid, 4321u);
    EXPECT_EQ(args->current_version, QStringLiteral("0.8.1"));
    EXPECT_EQ(args->base_url, QStringLiteral("http://localhost:8080"));
    EXPECT_EQ(args->preview_state, QStringLiteral("amber"));
}

TEST(UpdaterArgs, MissingInstallDirInPortableModeIsRejected) {
    QStringList argv{
        QStringLiteral("exosnap-updater.exe"), QStringLiteral("--install-mode"), QStringLiteral("portable"),
        QStringLiteral("--current-version"),   QStringLiteral("0.8.1"),
    };
    auto args = ParseUpdaterArgs(argv);
    EXPECT_FALSE(args.has_value());
}

TEST(UpdaterArgs, InstalledModeDoesNotRequireInstallDir) {
    QStringList argv{
        QStringLiteral("exosnap-updater.exe"),
        QStringLiteral("--install-mode"),
        QStringLiteral("installed"),
    };
    auto args = ParseUpdaterArgs(argv);
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(args->install_mode, InstallMode::Installed);
}

TEST(UpdaterArgs, ChannelParsing) {
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--channel"), QStringLiteral("stable"),
                         QStringLiteral("--install-mode"), QStringLiteral("installed")};
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->channel, UpdateChannel::Stable);
    }
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--channel"), QStringLiteral("bogus"),
                         QStringLiteral("--install-mode"), QStringLiteral("installed")};
        auto a = ParseUpdaterArgs(argv);
        EXPECT_FALSE(a.has_value());
    }
}

TEST(UpdaterArgs, PidParsing) {
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed"),
                         QStringLiteral("--app-pid"), QStringLiteral("99999")};
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->app_pid, 99999u);
    }
    {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed"),
                         QStringLiteral("--app-pid"), QStringLiteral("notanumber")};
        auto a = ParseUpdaterArgs(argv);
        EXPECT_FALSE(a.has_value());
    }
    {
        // Absent --app-pid means the app is not running (0).
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed")};
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value());
        EXPECT_EQ(a->app_pid, 0u);
    }
}

TEST(UpdaterArgs, MissingValueForFlagIsRejected) {
    QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode")};
    auto a = ParseUpdaterArgs(argv);
    EXPECT_FALSE(a.has_value());
}

// ── Verification reinstall (ADR 0055) ───────────────────────────────────────

TEST(UpdaterArgs, VerifyReinstallDefaultsOff) {
    auto args = ParseUpdaterArgs(FullArgLine());
    ASSERT_TRUE(args.has_value());
    EXPECT_FALSE(args->verify_reinstall);
}

TEST(UpdaterArgs, VerifyReinstallIsABooleanFlagAndConsumesNoValue) {
    QStringList argv{
        QStringLiteral("u"),         QStringLiteral("--verify-reinstall"), QStringLiteral("--install-mode"),
        QStringLiteral("installed"), QStringLiteral("--current-version"),  QStringLiteral("0.9.0-rc4"),
    };
    auto a = ParseUpdaterArgs(argv);
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a->verify_reinstall);
    EXPECT_EQ(a->install_mode, InstallMode::Installed) << "--verify-reinstall must not swallow the next flag";
    EXPECT_EQ(a->current_version, QStringLiteral("0.9.0-rc4"));
}

TEST(UpdaterArgs, VerifyReinstallWithoutCurrentVersionIsRejected) {
    QStringList argv{
        QStringLiteral("u"),
        QStringLiteral("--verify-reinstall"),
        QStringLiteral("--install-mode"),
        QStringLiteral("installed"),
    };
    EXPECT_FALSE(ParseUpdaterArgs(argv).has_value());
}

TEST(UpdaterArgs, InvalidPreviewStateIsRejected) {
    QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed"),
                     QStringLiteral("--preview-state"), QStringLiteral("teal")};
    auto a = ParseUpdaterArgs(argv);
    EXPECT_FALSE(a.has_value());
}

TEST(UpdaterArgs, EveryCanonPreviewStateIsAccepted) {
    // main.cpp renders from the same list this validates against. They used to
    // be two lists, and they had already drifted: main knew "download" and
    // "reboot" and the parser rejected both.
    for (const QString& name : PreviewStateNames()) {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed"),
                         QStringLiteral("--preview-state"), name};
        EXPECT_TRUE(ParseUpdaterArgs(argv).has_value()) << qPrintable(name);
    }
    EXPECT_TRUE(IsKnownPreviewState(QStringLiteral("download")));
    EXPECT_TRUE(IsKnownPreviewState(QStringLiteral("reboot")));
    EXPECT_FALSE(IsKnownPreviewState(QStringLiteral("teal")));
}

// -- mode ------------------------------------------------------------------

TEST(UpdaterArgs, NoArgumentsIsAManualStartAndIsAccepted) {
    // The whole point of the manual entry: a double-click used to die on the
    // --install-dir requirement, exit 2, and -- being a WIN32-subsystem binary
    // with no console -- show the user absolutely nothing.
    auto a = ParseUpdaterArgs({QStringLiteral("exosnap-updater.exe")});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->mode, exosnap::update::UpdaterMode::Manual);
    EXPECT_TRUE(a->install_dir.isEmpty()) << "a manual start derives its own context later";
    EXPECT_TRUE(a->target_version.isEmpty());
}

TEST(UpdaterArgs, ConfigurationAloneDoesNotArmTheHandoffPipeline) {
    // Channel and feed override are things a person may reasonably pass by hand,
    // so they must not turn a manual start into an unattended install.
    auto a = ParseUpdaterArgs({QStringLiteral("u"), QStringLiteral("--channel"), QStringLiteral("preview"),
                               QStringLiteral("--base-url"), QStringLiteral("https://localhost:8443/r")});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->mode, exosnap::update::UpdaterMode::Manual);
    EXPECT_EQ(a->channel, UpdateChannel::Preview);
}

TEST(UpdaterArgs, ContextArgumentsMakeItAHandoff) {
    for (const QStringList& tail : {QStringList{QStringLiteral("--install-dir"), QStringLiteral("C:/x")},
                                    QStringList{QStringLiteral("--app-pid"), QStringLiteral("7")},
                                    QStringList{QStringLiteral("--current-version"), QStringLiteral("0.9.0")},
                                    QStringList{QStringLiteral("--target-version"), QStringLiteral("0.9.1")}}) {
        QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed")};
        argv += tail;
        auto a = ParseUpdaterArgs(argv);
        ASSERT_TRUE(a.has_value()) << qPrintable(tail.join(QLatin1Char(' ')));
        EXPECT_EQ(a->mode, exosnap::update::UpdaterMode::LegacyHandoff) << qPrintable(tail.join(QLatin1Char(' ')));
    }
}

TEST(UpdaterArgs, PortableHandoffStillRequiresAnInstallDir) {
    // Unchanged for the handoff: it launched from a staged copy that is
    // deliberately NOT the installation, so it cannot derive the target itself.
    QStringList argv{
        QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("portable"), QStringLiteral("--app-pid"),
        QStringLiteral("7"),
    };
    EXPECT_FALSE(ParseUpdaterArgs(argv).has_value());
}

// -- --target-version -------------------------------------------------------

TEST(UpdaterArgs, TargetVersionRoundTrips) {
    QStringList argv{QStringLiteral("u"), QStringLiteral("--install-mode"), QStringLiteral("installed"),
                     QStringLiteral("--target-version"), QStringLiteral("0.9.0-rc5")};
    auto a = ParseUpdaterArgs(argv);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->target_version, QStringLiteral("0.9.0-rc5"));
}

TEST(UpdaterArgs, TargetVersionWithoutAValueIsRejected) {
    EXPECT_FALSE(ParseUpdaterArgs({QStringLiteral("u"), QStringLiteral("--target-version")}).has_value());
}

// -- manual context ---------------------------------------------------------

TEST(ResolveManualContextTest, InstalledCopyTrustsTheRegistryPath) {
    const ManualContext context =
        ResolveManualContext(InstallMode::Installed, QStringLiteral("C:/Program Files/Codexo/ExoSnap"),
                             QStringLiteral("C:/Users/u/AppData/Local/x/updater"));
    EXPECT_EQ(context.install_mode, InstallMode::Installed);
    EXPECT_EQ(context.install_dir, QStringLiteral("C:/Program Files/Codexo/ExoSnap"));
}

TEST(ResolveManualContextTest, InstalledCopyWithoutARegistryPathFallsBackToItsOwnDirectory) {
    const ManualContext context =
        ResolveManualContext(InstallMode::Installed, QString(), QStringLiteral("C:/Program Files/Codexo/ExoSnap/"));
    EXPECT_EQ(context.install_dir, QStringLiteral("C:/Program Files/Codexo/ExoSnap"))
        << "the path is cleaned, so a trailing separator does not become part of it";
}

TEST(ResolveManualContextTest, PortableUsesItsOwnDirectory) {
    // A MANUAL start runs from the installation. The staged handoff copy does
    // not, which is exactly why the mode is a field and not a derivation.
    const ManualContext context =
        ResolveManualContext(InstallMode::Portable, QStringLiteral("C:/ignored"), QStringLiteral("D:/Tools/ExoSnap"));
    EXPECT_EQ(context.install_mode, InstallMode::Portable);
    EXPECT_EQ(context.install_dir, QStringLiteral("D:/Tools/ExoSnap"));
}

TEST(ReadInstalledVersionTest, AnAbsentInstallDirectoryYieldsNothing) {
    EXPECT_TRUE(ReadInstalledVersion(QString()).isEmpty());
    EXPECT_TRUE(ReadInstalledVersion(QStringLiteral("D:/definitely/not/here")).isEmpty());
}

} // namespace
