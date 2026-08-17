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
#include "services/UpdateFeedOverride.h"
#include "services/UpdateService.h"
#include "services/VerifyReinstallMode.h"

#include <control/options.h>
#include <update/update_checker.h>

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

// -- BuildUpdaterArgs: two options, not a second contract -------------------
//
// The whole operation travels in the handoff document. The argv exists only to
// name it -- and, when this process is itself under automation, to arm the
// child's endpoint.

TEST(BuildUpdaterArgs, NamesTheHandoffAndNothingElse) {
    const QStringList flags = exosnap::BuildUpdaterArgs(QStringLiteral("C:/scratch/u-1/update-handoff.json"));
    ASSERT_EQ(flags.size(), 2);
    EXPECT_EQ(flags.at(0), QStringLiteral("--apply-handoff"));
    EXPECT_EQ(flags.at(1), QStringLiteral("C:/scratch/u-1/update-handoff.json"));
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

} // namespace

TEST(BuildUpdateHandoff, PinsTheOfferedVersionAndCarriesTheTrustAnchor) {
    upd::UpdateState st;
    st.install_mode = upd::InstallMode::Installed;
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
    st.install_mode = upd::InstallMode::Portable;
    st.available_version = upd::SemVer{0, 9, 0, true, 0};
    st.available_version_raw = "0.9.0-beta2";

    const auto handoff =
        exosnap::BuildUpdateHandoff(st, PreparedFor(QStringLiteral("0.9.0-beta2")), QStringLiteral("D:/x"), 1u,
                                    QStringLiteral("0.8.0"), /*verify_reinstall=*/false);
    EXPECT_EQ(handoff.target_version, QStringLiteral("0.9.0-beta2"));
}

TEST(BuildUpdateHandoff, VerificationReinstallPinsTheIdenticalVersion) {
    // Both gates then agree by construction: the target gate and the ADR 0055
    // gate compare the same string against the same manifest field.
    upd::UpdateState st;
    st.install_mode = upd::InstallMode::Portable;
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
    st.install_mode = upd::InstallMode::Installed;
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

// A stamp can only represent an accepted marked handoff in the current process.
// While it matches the available version, the card stays "pending".
TEST(ResolveUpdateCardState, PendingWhenStampMatchesAvailable) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QStringLiteral("2.0.0"),
                                              QStringLiteral("2.0.0")),
              QStringLiteral("pending"));
}

TEST(ResolveUpdateCardState, UpdaterProcessStartIsRunningNotRestartPending) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true, /*is_scoop=*/false, QString(), QStringLiteral("2.0.0"),
                  /*verify_reinstall_mode=*/false, QStringLiteral("1.0.0"),
                  exosnap::UpdateHandoffPhase::UpdaterRunning),
              QStringLiteral("updater-running"));
}

TEST(ResolveUpdateCardState, MarkedCloseHandoffIsTheOnlyRuntimePendingState) {
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true, /*is_scoop=*/false, QString(), QStringLiteral("2.0.0"),
                  /*verify_reinstall_mode=*/false, QStringLiteral("1.0.0"),
                  exosnap::UpdateHandoffPhase::ClosingForHandoff),
              QStringLiteral("pending"));
}

TEST(UpdateHandoffPersistence, LaunchFailureOrAbortLeavesNoAppliedVersion) {
    EXPECT_TRUE(exosnap::AppliedVersionForCommittedHandoff(QString(), false).isEmpty());
    EXPECT_EQ(exosnap::ResolveUpdateCardState(
                  /*update_available=*/true, /*is_scoop=*/false, QString(), QStringLiteral("2.0.0"),
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
    EXPECT_EQ(exosnap::ResolveUpdateCardState(/*update_available=*/true, /*is_scoop=*/false, QStringLiteral("2.0.0"),
                                              QStringLiteral("2.1.0")),
              QStringLiteral("available"));
}

// Recovery: a manual check clears any in-process handoff stamp before checking.
// With an empty stamp, the same still-applicable version re-arms to "available".
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
