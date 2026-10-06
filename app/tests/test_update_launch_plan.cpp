// test_update_launch_plan.cpp -- pure helpers behind UpdateService::LaunchUpdater.
//
// These cover the UI-agnostic staging/argument/guard logic so the actual
// CreateProcess + file-copy path in LaunchUpdater() stays a thin shell:
//   * UpdaterStagingFileList()  -- the files copied into the staged updater dir.
//   * BuildUpdaterArgs()        -- the argv the app hands the staged updater,
//                                  round-tripped through the updater's own parser.
//   * HandoffRefusalReason() -- trusted-offer and distribution eligibility.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QtGlobal>

#include "../apps/updater/UpdaterArgs.h"
#include "UpdaterExePath.h"
#include "services/UpdateFeedOverride.h"
#include "services/UpdateService.h"
#include "services/VerifyReinstallMode.h"

#include <control/options.h>
#include <update/update_checker.h>

namespace {

using exosnap::UpdateService;
namespace upd = exosnap::update;
using exosnap::updater::ParseUpdaterCommandLine;

// -- UpdaterStagingFileList -------------------------------------------------

TEST(UpdaterStagingFileList, CarriesTheQuickRuntimeTheUpdaterLinks) {
    const QStringList list = exosnap::UpdaterStagingFileList();
    EXPECT_TRUE(list.contains(QStringLiteral("exosnap-updater.exe")))
        << "staging list must include the updater executable";
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Core.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Gui.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Qml.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6Quick.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("Qt6QuickControls2.dll")));
    // The Widgets runtime is gone: the updater renders with Qt Quick now, and a
    // staged Qt6Widgets.dll nobody links is one more DLL to keep in step.
    EXPECT_FALSE(list.contains(QStringLiteral("Qt6Widgets.dll")));
}

TEST(UpdaterStagingFileList, IncludesPlatformPluginAndQmlImportTrees) {
    const QStringList list = exosnap::UpdaterStagingFileList();
    // The windows platform plugin is what lets the hidden/visible window exist
    // at all; the QML import trees carry the Controls style, its impl module and
    // the shape/layout modules the shared components import.
    EXPECT_TRUE(list.contains(QStringLiteral("plugins/platforms/qwindows.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("qml/QtQml/")));
    EXPECT_TRUE(list.contains(QStringLiteral("qml/QtQuick/Controls/qmldir")));
    EXPECT_TRUE(list.contains(QStringLiteral("qml/QtQuick/Controls/qtquickcontrols2plugin.dll")));
    EXPECT_TRUE(list.contains(QStringLiteral("qml/QtQuick/Controls/Basic/")));
    EXPECT_TRUE(list.contains(QStringLiteral("qml/QtQuick/Controls/impl/")));
    EXPECT_TRUE(list.contains(QStringLiteral("qml/QtQuick/Shapes/")));
    // Tooling metadata, not runtime payload: the official deploy tree carries no
    // .qmltypes file, so requiring one would refuse a correctly staged handoff.
    EXPECT_FALSE(list.contains(QStringLiteral("qml/QtQuick/Controls/plugins.qmltypes")));
    // No test runtime ever belongs in a staged product updater.
    for (const QString& entry : list)
        EXPECT_FALSE(entry.contains(QStringLiteral("Test"))) << entry.toStdString();
}

// -- BuildUpdaterArgs: two options, not a second contract -------------------
//
// The whole operation travels in the handoff document. The argv exists only to
// name it -- and, when this process is itself under automation, to arm the
// child's endpoint.

TEST(BuildUpdaterArgs, NamesTheHandoffAndPresentationLanguage) {
    const QStringList flags = exosnap::BuildUpdaterArgs(QStringLiteral("C:/scratch/u-1/update-handoff.json"));
    ASSERT_EQ(flags.size(), 4);
    EXPECT_EQ(flags.at(0), QStringLiteral("--apply-handoff"));
    EXPECT_EQ(flags.at(1), QStringLiteral("C:/scratch/u-1/update-handoff.json"));
    EXPECT_EQ(flags.at(2), QStringLiteral("--ui-language"));
    EXPECT_TRUE(flags.at(3) == QLatin1String("en") || flags.at(3) == QLatin1String("de"));
}

// Every removed search argument, named. Their absence is the point of the cut:
// two production spellings of one operation is what allowed the child to resolve
// a second release, and --base-url is what armed that resolution.
TEST(BuildUpdaterArgs, CarriesNoneOfTheFormerSearchArguments) {
    const QStringList flags = exosnap::BuildUpdaterArgs(QStringLiteral("C:/scratch/u-1/update-handoff.json"),
                                                        QStringLiteral("run-0123456789ab"));
    for (const QString& gone :
         {QStringLiteral("--channel"), QStringLiteral("--install-mode"), QStringLiteral("--install-dir"),
          QStringLiteral("--app-pid"), QStringLiteral("--current-version"), QStringLiteral("--target-version"),
          QStringLiteral("--verify-reinstall"), QStringLiteral("--base-url")}) {
        EXPECT_FALSE(flags.contains(gone)) << qPrintable(gone);
    }
}

// The argv the app writes must be an argv the updater accepts. Round-tripped
// through the updater's own parser, so the two cannot drift.
TEST(BuildUpdaterArgs, RoundTripsThroughTheUpdatersOwnParser) {
    QStringList argv;
    argv << QStringLiteral("exosnap-updater.exe");
    argv += exosnap::BuildUpdaterArgs(QStringLiteral("C:/scratch/u-1/update-handoff.json"));

    const auto parsed = ParseUpdaterCommandLine(argv);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->handoff_path, QStringLiteral("C:/scratch/u-1/update-handoff.json"));
    EXPECT_TRUE(parsed->base_url.isEmpty());
}

// -- the handoff document ----------------------------------------------------
//
// The truthfulness defect this closes: the app resolved the feed, told the user
// "version X is available", wrote a What's-new payload for X and stamped X into
// the applied-version loop guard -- and then the updater resolved the SAME feed
// again and installed whatever was newest at that second moment.

namespace {

UpdateService::PreparedUpdate PreparedFor(const QString& target) {
    UpdateService::PreparedUpdate prepared;
    prepared.update_transaction_id = QStringLiteral("u-0123456789abcdef");
    prepared.directory = QStringLiteral("C:/scratch/u-0123456789abcdef");
    prepared.manifest_path = QStringLiteral("C:/scratch/u-0123456789abcdef/update-manifest.json");
    prepared.manifest_signature_path = QStringLiteral("C:/scratch/u-0123456789abcdef/update-manifest.json.sig");
    prepared.target_version = target;
    return prepared;
}

TEST(DistributionHandoff, ManagedOwnersRefuseEvenACompletePreparedOffer) {
    namespace upd = exosnap::update;
    for (const auto mode : {upd::InstallMode::Portable, upd::InstallMode::Installed}) {
        for (const auto owner : {upd::DistributionOwner::WinGet, upd::DistributionOwner::Chocolatey,
                                 upd::DistributionOwner::Scoop, upd::DistributionOwner::UnknownManaged}) {
            upd::UpdateState state;
            state.distribution = {mode, owner};
            state.available_version_raw = "2.0.0";
            UpdateService::PreparedUpdate prepared;
            prepared.target_version = QStringLiteral("2.0.0");
            prepared.update_transaction_id = QStringLiteral("transaction");
            prepared.manifest_path = QStringLiteral("manifest.json");
            prepared.manifest_signature_path = QStringLiteral("manifest.sig");
            EXPECT_FALSE(exosnap::HandoffRefusalReason(state, prepared).isEmpty());
            EXPECT_EQ(exosnap::ResolveUpdateCardState(true, state.distribution, QString(), QStringLiteral("2.0.0")),
                      QStringLiteral("managed"));
        }
    }
}

} // namespace

TEST(BuildUpdateHandoff, PinsTheOfferedVersionAndCarriesTheTrustAnchor) {
    upd::UpdateState st;
    st.distribution.install_mode = upd::InstallMode::Installed;
    st.update_available = true;
    st.available_version = upd::SemVer{0, 9, 1};
    st.available_version_raw = "0.9.1";

    const auto handoff = exosnap::BuildUpdateHandoff(st, PreparedFor(QStringLiteral("0.9.1")),
                                                     QStringLiteral("C:/Program Files/Codexo/ExoSnap"), 4242u,
                                                     QStringLiteral("0.9.0"), /*verify_reinstall=*/false);
    EXPECT_EQ(handoff.handoff_version, exosnap::update_handoff::kHandoffVersion);
    EXPECT_EQ(handoff.target_version, QStringLiteral("0.9.1"));
    EXPECT_EQ(handoff.current_version, QStringLiteral("0.9.0"));
    EXPECT_EQ(handoff.install_mode, upd::InstallMode::Installed);
    EXPECT_EQ(handoff.install_dir, QStringLiteral("C:/Program Files/Codexo/ExoSnap"));
    EXPECT_EQ(handoff.app_pid, 4242u);
    EXPECT_FALSE(handoff.verify_reinstall);
    EXPECT_EQ(handoff.update_transaction_id, QStringLiteral("u-0123456789abcdef"));
    EXPECT_EQ(handoff.manifest_path, QStringLiteral("C:/scratch/u-0123456789abcdef/update-manifest.json"));
    EXPECT_EQ(handoff.manifest_signature_path,
              QStringLiteral("C:/scratch/u-0123456789abcdef/update-manifest.json.sig"));
}

TEST(BuildUpdateHandoff, PassesTheReleaseTagVerbatimNotAReSpelling) {
    // A foreign prerelease label survives as itself. SemVer::ToString() would
    // have rendered "0.9.0-beta2" as "0.9.0-rc0", and the manifest gate compares
    // strings -- so a re-spelled target would refuse the very release it pinned.
    upd::UpdateState st;
    st.distribution.install_mode = upd::InstallMode::Portable;
    st.available_version = upd::SemVer{0, 9, 0, true, 0};
    st.available_version_raw = "0.9.0-beta2";

    const auto handoff =
        exosnap::BuildUpdateHandoff(st, PreparedFor(QStringLiteral("0.9.0-beta2")), QStringLiteral("D:/x"), 1u,
                                    QStringLiteral("0.8.0"), /*verify_reinstall=*/false);
    EXPECT_EQ(handoff.target_version, QStringLiteral("0.9.0-beta2"));
}

TEST(BuildUpdateHandoff, VerificationReinstallPinsTheIdenticalVersion) {
    // Both gates then agree by construction: the target gate and the verification-reinstall
    // gate compare the same string against the same manifest field.
    upd::UpdateState st;
    st.distribution.install_mode = upd::InstallMode::Portable;
    st.available_version_raw = "0.9.0-rc4";

    const auto handoff =
        exosnap::BuildUpdateHandoff(st, PreparedFor(QStringLiteral("0.9.0-rc4")), QStringLiteral("D:/Tools/ExoSnap"),
                                    11u, QStringLiteral("0.9.0-rc4"), /*verify_reinstall=*/true);
    EXPECT_TRUE(handoff.verify_reinstall);
    EXPECT_EQ(handoff.target_version, handoff.current_version);
}

// A normal update run must never hand the updater the verification gate.
TEST(BuildUpdateHandoff, LeavesVerifyReinstallOffByDefault) {
    upd::UpdateState st;
    st.distribution.install_mode = upd::InstallMode::Installed;
    st.available_version_raw = "0.9.1";
    EXPECT_FALSE(exosnap::BuildUpdateHandoff(st, PreparedFor(QStringLiteral("0.9.1")), QStringLiteral("C:/x"), 1u,
                                             QStringLiteral("0.9.0"), /*verify_reinstall=*/false)
                     .verify_reinstall);
}

// -- when a handoff may be written at all -----------------------------------

TEST(HandoffRefusal, AcceptsAPreparationForTheOfferedVersion) {
    upd::UpdateState st;
    st.available_version_raw = "0.9.1";
    EXPECT_TRUE(exosnap::HandoffRefusalReason(st, PreparedFor(QStringLiteral("0.9.1"))).isEmpty());
}

TEST(HandoffRefusal, RefusesWhenNothingIsOnOffer) {
    upd::UpdateState st;
    EXPECT_FALSE(exosnap::HandoffRefusalReason(st, PreparedFor(QString())).isEmpty());
}

// The rule that keeps the offer and the transaction the same release: a
// preparation left over from a previous offer would hand the updater a
// transaction for a version the user never accepted.
TEST(HandoffRefusal, RefusesAPreparationForAnotherVersion) {
    upd::UpdateState st;
    st.available_version_raw = "0.9.1";
    const QString reason = exosnap::HandoffRefusalReason(st, PreparedFor(QStringLiteral("0.9.0")));
    EXPECT_FALSE(reason.isEmpty());
    EXPECT_TRUE(reason.contains(QStringLiteral("0.9.0")));
    EXPECT_TRUE(reason.contains(QStringLiteral("0.9.1")));
}

TEST(HandoffRefusal, RefusesWhenThePreparationFailedOrNeverRan) {
    upd::UpdateState st;
    st.available_version_raw = "0.9.1";

    UpdateService::PreparedUpdate failed = PreparedFor(QStringLiteral("0.9.1"));
    failed.error = QStringLiteral("Can't fetch the signed update manifest: HTTP 404");
    EXPECT_EQ(exosnap::HandoffRefusalReason(st, failed), failed.error)
        << "the apply must refuse with the reason the preparation recorded, not a generic one";

    EXPECT_FALSE(exosnap::HandoffRefusalReason(st, UpdateService::PreparedUpdate{}).isEmpty());

    UpdateService::PreparedUpdate without_manifest = PreparedFor(QStringLiteral("0.9.1"));
    without_manifest.manifest_signature_path.clear();
    EXPECT_FALSE(exosnap::HandoffRefusalReason(st, without_manifest).isEmpty());
}

// -- the child's automation endpoint ----------------------------------------

TEST(BuildUpdaterArgs, ArmsTheChildEndpointOnlyWhenTheParentHasOne) {
    const QStringList without = exosnap::BuildUpdaterArgs(QStringLiteral("C:/scratch/u-1/update-handoff.json"));
    EXPECT_FALSE(without.contains(QString::fromLatin1(exosnap::control::option::kUpdaterControl)))
        << "a normal launch must give the updater no endpoint at all";

    const QStringList with = exosnap::BuildUpdaterArgs(QStringLiteral("C:/scratch/u-1/update-handoff.json"),
                                                       QStringLiteral("run-0123456789ab"));
    const int index = with.indexOf(QString::fromLatin1(exosnap::control::option::kUpdaterControl));
    ASSERT_GE(index, 0);
    EXPECT_EQ(with.at(index + 1), QStringLiteral("run-0123456789ab"));
}

TEST(BuildUpdaterArgs, TheChildEndpointIsTheSameRunIdInADifferentRole) {
    // One run id, two roles: that is what lets a runner already driving the app
    // reach the updater it starts without minting or discovering anything.
    const QString run_id = QStringLiteral("run-0123456789ab");
    const QString app_pipe =
        exosnap::control::PipeName(QString::fromLatin1(exosnap::control::role::kApplication), run_id);
    const QString updater_pipe =
        exosnap::control::PipeName(QString::fromLatin1(exosnap::control::role::kUpdater), run_id);
    EXPECT_NE(app_pipe, updater_pipe);
    EXPECT_TRUE(updater_pipe.endsWith(run_id));
}

// -- --update-base-url --------------------------------------------------------

TEST(UpdateFeedOverride, IsAbsentUnlessPassed) {
    const auto override_ = exosnap::services::ParseUpdateFeedOverride({QStringLiteral("exosnap.exe")});
    EXPECT_FALSE(override_.requested);
    EXPECT_TRUE(override_.base_url.isEmpty());
    EXPECT_TRUE(override_.error.isEmpty());
}

TEST(UpdateFeedOverride, AcceptsAnHttpsUrlWithAHost) {
    EXPECT_TRUE(exosnap::services::IsAcceptableFeedUrl(QStringLiteral("https://localhost:8443/releases")));
    EXPECT_TRUE(exosnap::services::IsAcceptableFeedUrl(QStringLiteral("https://api.github.com/repos/x/y/releases")));
}

TEST(UpdateFeedOverride, RefusesAnythingThatIsNotHttpsWithAHost) {
    // FetchReleasesJson refuses these anyway; refusing here turns a typo into a
    // refused launch instead of a check that fails later with a network error
    // nobody connects to the command line.
    for (const QString& bad : {QStringLiteral("http://localhost/releases"), QStringLiteral("https://"),
                               QStringLiteral("localhost:8443"), QString()}) {
        EXPECT_FALSE(exosnap::services::IsAcceptableFeedUrl(bad)) << qPrintable(bad);
    }
}

TEST(UpdateFeedOverride, AMissingOrMalformedValueIsAnErrorNotAFallback) {
    // Falling back to the production feed would let a test believe it is pointed
    // at a fixture while it talks to GitHub -- and act on a real release.
    const auto missing = exosnap::services::ParseUpdateFeedOverride(
        {QStringLiteral("exosnap.exe"), QString::fromLatin1(exosnap::services::kUpdateFeedOverrideFlag)});
    EXPECT_TRUE(missing.requested);
    EXPECT_FALSE(missing.error.isEmpty());
    EXPECT_TRUE(missing.base_url.isEmpty());

    const auto malformed = exosnap::services::ParseUpdateFeedOverride(
        {QStringLiteral("exosnap.exe"), QString::fromLatin1(exosnap::services::kUpdateFeedOverrideFlag),
         QStringLiteral("http://localhost/releases")});
    EXPECT_TRUE(malformed.requested);
    EXPECT_FALSE(malformed.error.isEmpty());
    EXPECT_TRUE(malformed.base_url.isEmpty());
}

TEST(UpdateFeedOverride, IsAcceptedInThisBuildOnlyBecauseItIsNotOfficial) {
    // The rule, stated as a test rather than as a comment: an official build
    // refuses the flag outright, because a shipped artifact whose update source
    // can be redirected from a command line is a different product.
    const auto parsed = exosnap::services::ParseUpdateFeedOverride(
        {QStringLiteral("exosnap.exe"), QString::fromLatin1(exosnap::services::kUpdateFeedOverrideFlag),
         QStringLiteral("https://localhost:8443/releases")});
    ASSERT_TRUE(parsed.requested);
    if (exosnap::update::IsUpdateCheckEnabled()) {
        EXPECT_FALSE(parsed.error.isEmpty()) << "an official build must refuse the override";
        EXPECT_TRUE(parsed.base_url.isEmpty());
    } else {
        EXPECT_TRUE(parsed.error.isEmpty());
        EXPECT_EQ(parsed.base_url, QStringLiteral("https://localhost:8443/releases"));
    }
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

// -- ResolveUpdateCardState (loop guard + stuck-pending recovery) -----------

TEST(ResolveUpdateCardState, UpToDateWhenNoUpdate) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/false, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("2.0.0")),
              QStringLiteral("uptodate"));
}

TEST(ResolveUpdateCardState, ScoopWinsOverAvailable) {
    EXPECT_EQ(
        exosnap::ResolveUpdateCardState(/*update_available=*/true,
                                        exosnap::update::DistributionContext{exosnap::update::InstallMode::Portable,
                                                                             exosnap::update::DistributionOwner::Scoop},
                                        QString(), QStringLiteral("2.0.0")),
        QStringLiteral("managed"));
}

TEST(ResolveUpdateCardState, AvailableWhenNoStamp) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("2.0.0")),
              QStringLiteral("available"));
}

// A stamp can only represent an accepted marked handoff in the current process.
// While it matches the available version, the card stays "pending".
TEST(ResolveUpdateCardState, PendingWhenStampMatchesAvailable) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QStringLiteral("2.0.0"), QStringLiteral("2.0.0")),
              QStringLiteral("pending"));
}

TEST(ResolveUpdateCardState, UpdaterProcessStartIsRunningNotRestartPending) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true, exosnap::update::DistributionContext{}, QString(), QStringLiteral("2.0.0"),
                  /*verify_reinstall_mode=*/false, QStringLiteral("1.0.0"),
                  exosnap::UpdateHandoffPhase::UpdaterRunning),
              QStringLiteral("updater-running"));
}

TEST(ResolveUpdateCardState, MarkedCloseHandoffIsTheOnlyRuntimePendingState) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true, exosnap::update::DistributionContext{}, QString(), QStringLiteral("2.0.0"),
                  /*verify_reinstall_mode=*/false, QStringLiteral("1.0.0"),
                  exosnap::UpdateHandoffPhase::ClosingForHandoff),
              QStringLiteral("pending"));
}

TEST(UpdateHandoffPersistence, LaunchFailureOrAbortLeavesNoAppliedVersion) {
    EXPECT_TRUE(exosnap::AppliedVersionForCommittedHandoff(QString(), false).isEmpty());
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true, exosnap::update::DistributionContext{}, QString(), QStringLiteral("2.0.0"),
                  /*verify_reinstall_mode=*/false, QStringLiteral("1.0.0"), exosnap::UpdateHandoffPhase::Idle),
              QStringLiteral("available"));
}

TEST(UpdateHandoffPersistence, NormalCommittedHandoffStampsTarget) {
    EXPECT_EQ(exosnap::AppliedVersionForCommittedHandoff(QStringLiteral("2.0.0"), false), QStringLiteral("2.0.0"));
}

TEST(UpdateHandoffPersistence, VerifyReinstallNeverStampsSameVersion) {
    EXPECT_TRUE(exosnap::AppliedVersionForCommittedHandoff(QStringLiteral("0.9.0-rc4"), true).isEmpty());
}

TEST(UpdateHandoffPersistence, EveryFreshProcessDiscardsAStalePendingStamp) {
    EXPECT_TRUE(exosnap::ReconcileAppliedVersionOnStartup(QStringLiteral("2.0.0")).isEmpty());
    EXPECT_TRUE(exosnap::ReconcileAppliedVersionOnStartup(QStringLiteral("0.9.0-rc4")).isEmpty());
    EXPECT_TRUE(exosnap::ReconcileAppliedVersionOnStartup(QString()).isEmpty());
}

// A newer version than the stamped one is offered normally.
TEST(ResolveUpdateCardState, AvailableWhenStampIsOlderVersion) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QStringLiteral("2.0.0"), QStringLiteral("2.1.0")),
              QStringLiteral("available"));
}

// Recovery: a manual check clears any in-process handoff stamp before checking.
// With an empty stamp, the same still-applicable version re-arms to "available".
TEST(ResolveUpdateCardState, RearmsToAvailableAfterManualCheckClearsStamp) {
    // Automatic re-check with the stamp still set -> pending.
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QStringLiteral("2.0.0"), QStringLiteral("2.0.0")),
              QStringLiteral("pending"));
    // Manual check clears the stamp upstream; resolver now sees an empty stamp.
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("2.0.0")),
              QStringLiteral("available"));
}

// -- ResolveUpdateCardState: verification reinstall --------------

TEST(ResolveUpdateCardState, VerifyReinstallWhenModeIsOnAndTheOfferIsTheRunningVersion) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("verify-reinstall"));
}

// The mode does not turn every offer into a reinstall: a genuinely newer release
// is still a normal update.
TEST(ResolveUpdateCardState, AvailableWhenVerifyModeIsOnButTheOfferIsNewer) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("0.9.0"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("available"));
}

// Without the mode, an offer equal to the running version cannot reach the
// reinstall state at all (the engine would not offer it in the first place).
TEST(ResolveUpdateCardState, NoVerifyReinstallWhenTheModeIsOff) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/false,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("available"));
}

// Scoop trees are never touched by the staged swap — not even in verify mode.
TEST(ResolveUpdateCardState, ScoopStillWinsInVerifyMode) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true,
                  exosnap::update::DistributionContext{exosnap::update::InstallMode::Portable,
                                                       exosnap::update::DistributionOwner::Scoop},
                  QString(), QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/true, QStringLiteral("0.9.0-rc4")),
              QStringLiteral("managed"));
}

TEST(ResolveUpdateCardState, UpToDateStillWinsInVerifyMode) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/false, exosnap::update::DistributionContext{},
                                              QString(), QStringLiteral("0.9.0-rc4"), /*verify_reinstall_mode=*/true,
                                              QStringLiteral("0.9.0-rc4")),
              QStringLiteral("uptodate"));
}

// The loop guard exists to stop a stale cache from re-offering an update that is
// already staged. Re-running the swap for the SAME version is exactly what
// verification mode is for, so it outranks the guard.
TEST(ResolveUpdateCardState, VerifyReinstallOutranksThePendingLoopGuard) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc4"),
                                              /*verify_reinstall_mode=*/true, QStringLiteral("0.9.0-rc4")),
              QStringLiteral("verify-reinstall"));
}

TEST(ResolveUpdateCardState, VerifyModeWithoutAnOfferedVersionFallsBack) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, exosnap::update::DistributionContext{},
                                              QString(), QString(),
                                              /*verify_reinstall_mode=*/true, QString()),
              QStringLiteral("available"));
}

} // namespace

// -- UpdaterChildEnvironment: the app's own rendering opt-out is not the
//    updater's ----------------------------------------------------------------
//
// The main app's rendering opt-out must not leak into the independent Qt Quick
// updater. Its QPA surface is configured by its own process.

TEST(UpdaterChildEnvironment, DropsTheRedirectionSurfaceOptOut) {
    QProcessEnvironment parent;
    parent.insert(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE"), QStringLiteral("1"));

    const QProcessEnvironment child = exosnap::UpdaterChildEnvironment(parent);

    EXPECT_FALSE(child.contains(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE")));
}

TEST(UpdaterChildEnvironment, KeepsEverythingElse) {
    QProcessEnvironment parent;
    parent.insert(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE"), QStringLiteral("1"));
    parent.insert(QStringLiteral("PATH"), QStringLiteral("C:/Qt/bin"));
    parent.insert(QStringLiteral("EXOSNAP_CONFIG_DIR"), QStringLiteral("C:/scratch/cfg"));

    const QProcessEnvironment child = exosnap::UpdaterChildEnvironment(parent);

    EXPECT_EQ(child.value(QStringLiteral("PATH")), QStringLiteral("C:/Qt/bin"));
    EXPECT_EQ(child.value(QStringLiteral("EXOSNAP_CONFIG_DIR")), QStringLiteral("C:/scratch/cfg"));
}

// An environment that never had the variable must come back unchanged rather than
// gaining an empty entry: QProcessEnvironment::remove on an absent key is a no-op,
// and this asserts the function does not work around it.
TEST(UpdaterChildEnvironment, LeavesAnEnvironmentWithoutTheKeyAlone) {
    QProcessEnvironment parent;
    parent.insert(QStringLiteral("PATH"), QStringLiteral("C:/Qt/bin"));

    const QProcessEnvironment child = exosnap::UpdaterChildEnvironment(parent);

    EXPECT_FALSE(child.contains(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE")));
    EXPECT_EQ(child.keys().size(), parent.keys().size());
}

// The composition the product actually performs. UpdaterChildEnvironment is pure,
// but the fix only works if the environment handed to it REFLECTS the qputenv
// main.cpp did -- otherwise the key would be dropped by luck rather than by
// design, and a later change to how the app sets it would go unnoticed.
TEST(UpdaterChildEnvironment, DropsTheKeyThisProcessSetWithQputenv) {
    const bool had = qEnvironmentVariableIsSet("QT_QPA_DISABLE_REDIRECTION_SURFACE");
    const QByteArray previous = qgetenv("QT_QPA_DISABLE_REDIRECTION_SURFACE");
    qputenv("QT_QPA_DISABLE_REDIRECTION_SURFACE", "1");
    // The premise: without this the test below would pass for the wrong reason.
    ASSERT_TRUE(
        QProcessEnvironment::systemEnvironment().contains(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE")));

    const QProcessEnvironment child = exosnap::UpdaterChildEnvironment(QProcessEnvironment::systemEnvironment());
    EXPECT_FALSE(child.contains(QStringLiteral("QT_QPA_DISABLE_REDIRECTION_SURFACE")));

    if (had) {
        qputenv("QT_QPA_DISABLE_REDIRECTION_SURFACE", previous);
    } else {
        qunsetenv("QT_QPA_DISABLE_REDIRECTION_SURFACE");
    }
}

// -- Staged updater runtime: self-contained launch ---------------------------
//
// The real proof of the deployment contract: copy exactly the files the product
// stages into a clean directory, strip the environment down to Windows itself,
// and launch the shipped updater from there. A missing QML import, a missing
// runtime DLL or a dependency on the live install tree fails the launch rather
// than hiding behind a developer PATH.
//
// This test runs in Debug as well as Release, and a Debug build tree carries
// `Qt6Cored.dll` where the product list names `Qt6Core.dll`. The staging loop
// below therefore resolves the debug spelling when the release one is absent;
// the PRODUCT list stays release-only, because the updater never runs from a
// build tree.

namespace {

QString RuntimeSourceName(const QString& app_dir, const QString& relative) {
    if (QFile::exists(QDir(app_dir).filePath(relative)))
        return relative;
    if (relative.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive)) {
        QString debug_relative = relative;
        debug_relative.chop(4);
        debug_relative += QStringLiteral("d.dll");
        if (QFile::exists(QDir(app_dir).filePath(debug_relative)))
            return debug_relative;
    }
    // The install tree spells the plugin root `plugins/platforms/...`; a
    // windeployqt output directory next to the build-tree exe is flat
    // (`platforms/...`). The PRODUCT layout is the install one; this only maps
    // the build-tree source.
    if (relative.startsWith(QStringLiteral("plugins/"))) {
        const QString flat = relative.mid(QStringLiteral("plugins/").size());
        if (QFile::exists(QDir(app_dir).filePath(flat)))
            return flat;
        if (flat.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive)) {
            QString flat_debug = flat;
            flat_debug.chop(4);
            flat_debug += QStringLiteral("d.dll");
            if (QFile::exists(QDir(app_dir).filePath(flat_debug)))
                return flat_debug;
        }
    }
    return relative;
}

bool StageForTest(const QString& app_dir, const QString& stage, QString* error) {
    for (const QString& rel : exosnap::UpdaterStagingFileList()) {
        const bool directory = rel.endsWith(QLatin1Char('/'));
        const QString relative = directory ? rel.left(rel.size() - 1) : rel;
        const QString source_path = QDir(app_dir).filePath(RuntimeSourceName(app_dir, relative));
        if (directory) {
            if (!QFileInfo(source_path).isDir()) {
                *error = QStringLiteral("missing directory %1").arg(relative);
                return false;
            }
            QDirIterator it(source_path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString file = it.next();
                const QString within = QDir(source_path).relativeFilePath(file);
                const QString destination = QDir(stage).filePath(relative + QLatin1Char('/') + within);
                QDir().mkpath(QFileInfo(destination).absolutePath());
                if (!QFile::copy(file, destination)) {
                    *error = QStringLiteral("failed to copy %1").arg(within);
                    return false;
                }
            }
            continue;
        }
        if (!QFileInfo::exists(source_path)) {
            *error = QStringLiteral("missing %1").arg(relative);
            return false;
        }
        // The destination keeps the SOURCE basename: in Debug that is the
        // `...d.dll` spelling the debug Qt libraries import by name, so the
        // staged tree stays internally coherent. The product's release names are
        // pinned by the staging-list tests above.
        const QString source_name = QFileInfo(source_path).fileName();
        const QString parent = QFileInfo(relative).path();
        const QString destination_relative =
            parent.isEmpty() || parent == QLatin1String(".") ? source_name : parent + QLatin1Char('/') + source_name;
        const QString destination = QDir(stage).filePath(destination_relative);
        QDir().mkpath(QFileInfo(destination).absolutePath());
        if (!QFile::copy(source_path, destination)) {
            *error = QStringLiteral("failed to copy %1").arg(relative);
            return false;
        }
    }
    return true;
}

bool HasPlatformPluginAt(const QString& root, const QString& debug_name, const QString& release_name) {
    const QDir dir(QDir(root).filePath(QStringLiteral("plugins/platforms")));
    return dir.exists(debug_name) || dir.exists(release_name);
}

} // namespace

TEST(StageUpdaterRuntime, ReportsAMissingSourceDirectory) {
    QTemporaryDir stage_dir;
    ASSERT_TRUE(stage_dir.isValid());
    QString error;
    EXPECT_FALSE(exosnap::StageUpdaterRuntime(QDir(stage_dir.path()).filePath(QStringLiteral("nowhere")),
                                              stage_dir.path(), &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(StageUpdaterRuntime, ProducesASelfContainedQuickRuntimeThatLaunches) {
    const QString app_dir = QFileInfo(QString::fromUtf8(EXOSNAP_UPDATER_EXE)).absolutePath();
    ASSERT_TRUE(QFileInfo(app_dir).isDir()) << app_dir.toStdString();

    QTemporaryDir stage_dir;
    ASSERT_TRUE(stage_dir.isValid());
    const QString stage = stage_dir.path();

    QString error;
    ASSERT_TRUE(StageForTest(app_dir, stage, &error)) << error.toStdString();

    // The Quick runtime and the QML import trees arrived, and no test runtime
    // did.
    EXPECT_TRUE(QFile::exists(QDir(stage).filePath(QStringLiteral("exosnap-updater.exe"))));
    EXPECT_TRUE(QFile::exists(
        QDir(stage).filePath(QFileInfo(RuntimeSourceName(app_dir, QStringLiteral("Qt6Quick.dll"))).fileName())));
    EXPECT_TRUE(QFile::exists(QDir(stage).filePath(QStringLiteral("qml/QtQuick/Controls/Basic/qmldir"))));
    EXPECT_TRUE(QFile::exists(QDir(stage).filePath(QStringLiteral("qml/QtQuick/Controls/impl/qmldir"))));
    EXPECT_TRUE(HasPlatformPluginAt(stage, QStringLiteral("qwindowsd.dll"), QStringLiteral("qwindows.dll")));
    for (const QFileInfo& file : QDir(stage).entryInfoList(QStringList{QStringLiteral("*.dll")}, QDir::Files))
        EXPECT_FALSE(file.fileName().contains(QStringLiteral("Test"))) << file.fileName().toStdString();

    // Test-only: the offscreen platform plugin, so this check renders without a
    // visible window. Deliberately NOT in UpdaterStagingFileList().
    const QString test_platform = QDir(app_dir).filePath(QStringLiteral("platforms/qoffscreend.dll"));
    const QString test_platform_release = QDir(app_dir).filePath(QStringLiteral("platforms/qoffscreen.dll"));
    const QString platform_source = QFile::exists(test_platform) ? test_platform : test_platform_release;
    ASSERT_TRUE(QFile::exists(platform_source));
    QDir().mkpath(QDir(stage).filePath(QStringLiteral("plugins/platforms")));
    ASSERT_TRUE(QFile::copy(platform_source, QDir(stage).filePath(QStringLiteral("plugins/platforms/") +
                                                                  QFileInfo(platform_source).fileName())));

    // Test-only: a Debug build needs the debug CRT, which a user machine never
    // receives from the product (release CRTs come from the VC redistributable).
    // The build tree carries it; a Release run simply copies nothing.
    for (const QFileInfo& file :
         QDir(app_dir).entryInfoList(QStringList{QStringLiteral("msvcp*d.dll"), QStringLiteral("vcruntime*d.dll"),
                                                 QStringLiteral("concrt*d.dll"), QStringLiteral("ucrtbased.dll")},
                                     QDir::Files)) {
        QFile::copy(file.absoluteFilePath(), QDir(stage).filePath(file.fileName()));
    }

    QFile qt_conf(QDir(stage).filePath(QStringLiteral("qt.conf")));
    ASSERT_TRUE(qt_conf.open(QIODevice::WriteOnly | QIODevice::Truncate));
    qt_conf.write("[Paths]\nPlugins = plugins\n");
    qt_conf.close();

    // Sanitized: Windows itself and nothing that could point back at the live
    // install tree or a developer Qt.
    QProcessEnvironment env;
    const QString system_root = QString::fromLocal8Bit(qgetenv("SystemRoot"));
    env.insert(QStringLiteral("PATH"), QStringLiteral("%1\\system32;%1").arg(system_root));
    env.insert(QStringLiteral("SystemRoot"), system_root);
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));

    QProcess process;
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(stage);
    process.start(QDir(stage).filePath(QStringLiteral("exosnap-updater.exe")),
                  {QStringLiteral("--preview-state"), QStringLiteral("progress"), QStringLiteral("--preview-smoke")});
    ASSERT_TRUE(process.waitForStarted(10000));
    ASSERT_TRUE(process.waitForFinished(30000)) << process.readAllStandardError().toStdString();
    EXPECT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
}

TEST(BuildUpdaterArgs, PreservesTheRunningEffectiveLanguage) {
    static int argc = 1;
    static char name[] = "update_launch_plan_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application(argc, argv);
    QCoreApplication* current = QCoreApplication::instance();
    ASSERT_NE(current, nullptr);
    const QVariant previous = current->property("exosnapEffectiveLanguage");
    current->setProperty("exosnapEffectiveLanguage", QStringLiteral("de"));
    const QStringList flags = exosnap::BuildUpdaterArgs(QStringLiteral("handoff.json"));
    const int flag = flags.indexOf(QStringLiteral("--ui-language"));
    ASSERT_GE(flag, 0);
    EXPECT_EQ(flags.value(flag + 1), QStringLiteral("de"));
    current->setProperty("exosnapEffectiveLanguage", previous);
}