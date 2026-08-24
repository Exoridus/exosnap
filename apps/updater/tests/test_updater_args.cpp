// Unit tests for the updater command line and the handoff projection.
//
// The contract these pin: the command line decides the MODE and nothing else.
// Everything about an update operation arrives in the handoff document, and
// nothing about it can be spelled on the command line any more -- that second,
// unversioned spelling is precisely what was removed.

#include <gtest/gtest.h>

#include "UpdaterArgs.h"

using namespace exosnap::updater;

namespace {

using exosnap::update::InstallMode;
using exosnap::update::UpdateChannel;
using exosnap::update::UpdaterMode;
using exosnap::update_handoff::UpdateHandoff;

UpdateHandoff SampleHandoff() {
    UpdateHandoff handoff;
    handoff.update_transaction_id = QStringLiteral("u-0123456789abcdef");
    handoff.target_version = QStringLiteral("0.9.0-rc9");
    handoff.current_version = QStringLiteral("0.9.0-rc1");
    handoff.manifest_path = QStringLiteral("C:/scratch/u-1/update-manifest.json");
    handoff.manifest_signature_path = QStringLiteral("C:/scratch/u-1/update-manifest.json.sig");
    handoff.install_mode = InstallMode::Portable;
    handoff.install_dir = QStringLiteral("C:/Apps/ExoSnap");
    handoff.app_pid = 4321;
    handoff.verify_reinstall = false;
    return handoff;
}

// ── The command line ────────────────────────────────────────────────────────

TEST(UpdaterCommandLine, NoArgumentsIsAManualStartAndIsAccepted) {
    // The whole point of the manual entry: a double-click used to die on the
    // --install-dir requirement, exit 2, and -- being a WIN32-subsystem binary
    // with no console -- show the user absolutely nothing.
    const auto parsed = ParseUpdaterCommandLine({QStringLiteral("exosnap-updater.exe")});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->handoff_path.isEmpty());
    EXPECT_EQ(ArgsForManualStart(*parsed).mode, UpdaterMode::Manual);
}

TEST(UpdaterCommandLine, ApplyHandoffNamesTheDocument) {
    const auto parsed = ParseUpdaterCommandLine(
        {QStringLiteral("u"), QStringLiteral("--apply-handoff"), QStringLiteral("C:/scratch/u-1/update-handoff.json")});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->handoff_path, QStringLiteral("C:/scratch/u-1/update-handoff.json"));
}

TEST(UpdaterCommandLine, ConfigurationAloneDoesNotArmTheHandoffPipeline) {
    // Channel and feed override are things a person may reasonably pass by hand,
    // so they must not turn a manual start into an unattended install.
    const auto parsed =
        ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--channel"), QStringLiteral("preview"),
                                 QStringLiteral("--base-url"), QStringLiteral("https://localhost:8443/r")});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->handoff_path.isEmpty());
    const UpdaterArgs args = ArgsForManualStart(*parsed);
    EXPECT_EQ(args.mode, UpdaterMode::Manual);
    EXPECT_EQ(args.channel, UpdateChannel::Preview);
    EXPECT_EQ(args.base_url, QStringLiteral("https://localhost:8443/r"));
}

// The search arguments are GONE, not deprecated. Two production spellings of one
// operation is exactly the state this cut removed, so every one of them has to
// be an unknown argument rather than a tolerated alias.
TEST(UpdaterCommandLine, TheFormerSearchArgumentsAreNoLongerAccepted) {
    const QStringList removed[] = {
        {QStringLiteral("--install-mode"), QStringLiteral("portable")},
        {QStringLiteral("--install-dir"), QStringLiteral("C:/Apps/ExoSnap")},
        {QStringLiteral("--app-pid"), QStringLiteral("4321")},
        {QStringLiteral("--current-version"), QStringLiteral("0.8.1")},
        {QStringLiteral("--target-version"), QStringLiteral("0.9.0")},
        {QStringLiteral("--verify-reinstall")},
    };
    for (const QStringList& tail : removed) {
        QStringList argv{QStringLiteral("u")};
        argv += tail;
        EXPECT_FALSE(ParseUpdaterCommandLine(argv).has_value()) << qPrintable(tail.join(QLatin1Char(' ')));
    }
}

TEST(UpdaterCommandLine, ChannelParsing) {
    const auto stable =
        ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--channel"), QStringLiteral("stable")});
    ASSERT_TRUE(stable.has_value());
    EXPECT_EQ(stable->channel, UpdateChannel::Stable);
    EXPECT_FALSE(ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--channel"), QStringLiteral("bogus")})
                     .has_value());
}

TEST(UpdaterCommandLine, MissingValueForFlagIsRejected) {
    EXPECT_FALSE(ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--channel")}).has_value());
    EXPECT_FALSE(ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--apply-handoff")}).has_value());
}

TEST(UpdaterCommandLine, InvalidPreviewStateIsRejected) {
    EXPECT_FALSE(
        ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--preview-state"), QStringLiteral("teal")})
            .has_value());
}

TEST(UpdaterCommandLine, EveryCanonPreviewStateIsAccepted) {
    // main.cpp renders from the same list this validates against. They used to
    // be two lists, and they had already drifted: main knew "download" and
    // "reboot" and the parser rejected both.
    for (const QString& name : PreviewStateNames()) {
        EXPECT_TRUE(ParseUpdaterCommandLine({QStringLiteral("u"), QStringLiteral("--preview-state"), name}).has_value())
            << qPrintable(name);
    }
    EXPECT_TRUE(IsKnownPreviewState(QStringLiteral("download")));
    EXPECT_TRUE(IsKnownPreviewState(QStringLiteral("reboot")));
    EXPECT_FALSE(IsKnownPreviewState(QStringLiteral("teal")));
}

// ── The handoff projection ──────────────────────────────────────────────────

TEST(ArgsFromHandoffTest, CarriesEveryFieldOfTheOperation) {
    UpdaterCommandLine command_line;
    const UpdaterArgs args = ArgsFromHandoff(SampleHandoff(), command_line);

    EXPECT_EQ(args.mode, UpdaterMode::AppHandoff);
    EXPECT_EQ(args.install_mode, InstallMode::Portable);
    EXPECT_EQ(args.install_dir, QStringLiteral("C:/Apps/ExoSnap"));
    EXPECT_EQ(args.app_pid, 4321u);
    EXPECT_EQ(args.current_version, QStringLiteral("0.9.0-rc1"));
    EXPECT_EQ(args.target_version, QStringLiteral("0.9.0-rc9"));
    EXPECT_EQ(args.update_transaction_id, QStringLiteral("u-0123456789abcdef"));
    EXPECT_EQ(args.manifest_path, QStringLiteral("C:/scratch/u-1/update-manifest.json"));
    EXPECT_EQ(args.manifest_signature_path, QStringLiteral("C:/scratch/u-1/update-manifest.json.sig"));
    EXPECT_FALSE(args.verify_reinstall);
}

TEST(ArgsFromHandoffTest, CarriesTheVerificationReinstallGate) {
    UpdateHandoff handoff = SampleHandoff();
    handoff.verify_reinstall = true;
    handoff.target_version = handoff.current_version;
    const UpdaterArgs args = ArgsFromHandoff(handoff, UpdaterCommandLine{});
    EXPECT_TRUE(args.verify_reinstall);
    EXPECT_EQ(args.target_version, args.current_version);
}

// A handoff run resolves no feed. Its release is pinned and its manifest was
// handed over, so a channel and a base URL would be inputs to a search that does
// not happen -- and a base URL in particular would re-arm the second resolution
// the pinned target only ever existed to compensate for.
TEST(ArgsFromHandoffTest, IgnoresFeedConfigurationFromTheCommandLine) {
    UpdaterCommandLine command_line;
    command_line.channel = UpdateChannel::Preview;
    command_line.base_url = QStringLiteral("https://localhost:8443/r");
    const UpdaterArgs args = ArgsFromHandoff(SampleHandoff(), command_line);
    EXPECT_TRUE(args.base_url.isEmpty());
    EXPECT_EQ(args.channel, UpdateChannel::Stable) << "the default, because no channel is resolved at all";
}

TEST(ArgsForManualStartTest, CarriesNoOperation) {
    const UpdaterArgs args = ArgsForManualStart(UpdaterCommandLine{});
    EXPECT_TRUE(args.target_version.isEmpty());
    EXPECT_TRUE(args.update_transaction_id.isEmpty());
    EXPECT_TRUE(args.manifest_path.isEmpty());
    EXPECT_TRUE(args.manifest_signature_path.isEmpty());
    EXPECT_EQ(args.app_pid, 0u);
    EXPECT_FALSE(args.verify_reinstall);
}

// ── manual context ──────────────────────────────────────────────────────────

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
