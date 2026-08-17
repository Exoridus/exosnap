// Unit tests for the pure updater step state machine.
//
// No QApplication: UpdaterController is Qt-Core value logic only.

#include <gtest/gtest.h>

#include "UpdaterController.h"

namespace {

UpdaterController MakeController() {
    return UpdaterController(QStringLiteral("0.8.1"), QStringLiteral("0.9.0"));
}

TEST(UpdaterController, InitialStateIsAllQueuedRingZero) {
    UpdaterController c = MakeController();
    const UpdaterUiState& s = c.state();

    for (StepStatus st : s.steps) {
        EXPECT_EQ(st, StepStatus::Queued);
    }
    EXPECT_DOUBLE_EQ(s.ring, 0.0);
    EXPECT_EQ(s.variant, TerminalVariant::None);
    EXPECT_EQ(s.from_version, QStringLiteral("0.8.1"));
    EXPECT_EQ(s.to_version, QStringLiteral("0.9.0"));
    EXPECT_TRUE(s.primary_action.isEmpty());
    EXPECT_TRUE(s.secondary_action.isEmpty());
}

TEST(UpdaterController, HappySequenceEndsSuccessRingFull) {
    UpdaterController c = MakeController();
    for (int i = 0; i < int(UpStep::Count); ++i) {
        const UpStep step = static_cast<UpStep>(i);
        c.onStepStarted(step);
        c.onStepDone(step);
    }
    c.onAllDone();

    const UpdaterUiState& s = c.state();
    for (StepStatus st : s.steps) {
        EXPECT_EQ(st, StepStatus::Done);
    }
    EXPECT_DOUBLE_EQ(s.ring, 1.0);
    EXPECT_EQ(s.variant, TerminalVariant::Success);
}

TEST(UpdaterController, StepStartedMarksWorkingAndStatusLine) {
    UpdaterController c = MakeController();
    c.onStepStarted(UpStep::Download);
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.steps[size_t(UpStep::Download)], StepStatus::Working);
    EXPECT_TRUE(s.status_line.contains(QStringLiteral("0.9.0")));
    EXPECT_TRUE(s.status_line.contains(QStringLiteral("Downloading")));
}

TEST(UpdaterController, StepDoneSnapsRingToWeight) {
    UpdaterController c = MakeController();
    c.onStepDone(UpStep::Download);
    EXPECT_NEAR(c.state().ring, 0.55, 1e-9);
    c.onStepDone(UpStep::CloseApp);
    EXPECT_NEAR(c.state().ring, 0.62, 1e-9);
    c.onStepDone(UpStep::Install);
    EXPECT_NEAR(c.state().ring, 0.85, 1e-9);
    c.onStepDone(UpStep::Verify);
    EXPECT_NEAR(c.state().ring, 0.94, 1e-9);
    c.onStepDone(UpStep::Launch);
    EXPECT_NEAR(c.state().ring, 1.0, 1e-9);
}

TEST(UpdaterController, DownloadProgressScalesWithinBand) {
    UpdaterController c = MakeController();
    c.onStepStarted(UpStep::Download);
    c.onDownloadProgress(50, 100);
    EXPECT_NEAR(c.state().ring, 0.275, 1e-9); // 0.55 * 0.5
}

TEST(UpdaterController, DownloadProgressZeroTotalIsSafe) {
    UpdaterController c = MakeController();
    c.onStepStarted(UpStep::Download);
    c.onDownloadProgress(0, 0);
    EXPECT_GE(c.state().ring, 0.0);
    EXPECT_LE(c.state().ring, 0.55);
}

TEST(UpdaterController, VerifyInstallFailedIsRedRestored) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::VerifyInstallFailed, QString());

    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.steps[size_t(UpStep::Verify)], StepStatus::Failed);
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("previous version 0.8.1 was restored")));
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Close"));
}

TEST(UpdaterController, VerifyInstallFailedMsiDoesNotClaimAConfirmedRollback) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::VerifyInstallFailedMsi, QString());

    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.steps[size_t(UpStep::Verify)], StepStatus::Failed);
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    // MSI has no portable-style backup, so the portable "was restored" wording (which
    // asserts a specific outcome) must never appear here. But runVerify() never queries
    // Windows Installer for an actual rollback outcome either -- it only re-reads the
    // registry install path and re-checks the version -- so the copy must not assert a
    // rollback happened, only that the post-install state could not be confirmed.
    EXPECT_TRUE(s.headline.contains(QStringLiteral("confirm")));
    EXPECT_FALSE(s.safety_text.contains(QStringLiteral("was restored")));
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Close"));
}

TEST(UpdaterController, MsiRebootRequiredIsTerminalSuccessWithRestartCopy) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::MsiRebootRequired, QString());

    const UpdaterUiState& s = c.state();
    // The upgrade applied: the Install step is Done, not Failed, and this is its
    // own terminal variant rather than a red failure card.
    EXPECT_EQ(s.steps[size_t(UpStep::Install)], StepStatus::Done);
    EXPECT_EQ(s.variant, TerminalVariant::RebootRequired);
    EXPECT_TRUE(s.headline.contains(QStringLiteral("restart Windows")));
    EXPECT_TRUE(s.detail_text.contains(QStringLiteral("system restart")));
    // Not the generic C2 failure wording.
    EXPECT_FALSE(s.headline.contains(QStringLiteral("Installation failed")));
    EXPECT_EQ(s.primary_action, QStringLiteral("Close"));
    EXPECT_TRUE(s.secondary_action.isEmpty());
}

TEST(UpdaterController, LaunchFailedIsGreenSoftSuccess) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::LaunchFailed, QString());

    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Green);
    EXPECT_EQ(s.primary_action, QStringLiteral("Open ExoSnap"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Close"));
    // %1 in the copy resolves to the target version.
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("0.9.0")));
}

TEST(UpdaterController, DownloadFailedIsAmber) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::DownloadFailed, QString());
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Amber);
    EXPECT_EQ(s.steps[size_t(UpStep::Download)], StepStatus::Failed);
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Close"));
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("current version 0.8.1 is unchanged")));
    EXPECT_FALSE(s.detail_text.contains(QStringLiteral("WinHttp")));
}

TEST(UpdaterController, VerifyDownloadFailedIsRedSecurityStop) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::VerifyDownloadFailed, QString());
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.primary_action, QStringLiteral("Re-download"));
    EXPECT_TRUE(s.detail_text.contains(QStringLiteral("signed release")));
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("Nothing was installed")));
}

TEST(UpdaterController, MsiFailedEmbedsCodeAndHasNoSecondary) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::MsiFailed, QStringLiteral("1603"));
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.primary_action, QStringLiteral("Close"));
    EXPECT_TRUE(s.secondary_action.isEmpty());
    EXPECT_TRUE(s.detail_text.contains(QStringLiteral("1603")));
}

// ── Verification reinstall (ADR 0055) ───────────────────────────────────────

TEST(UpdaterController, VerificationReinstallIsOffByDefault) {
    UpdaterController c = MakeController();
    EXPECT_FALSE(c.state().verification_reinstall);
    c.onStepStarted(UpStep::Install);
    EXPECT_TRUE(c.state().status_line.contains(QStringLiteral("Swapping in version")));
}

TEST(UpdaterController, VerificationReinstallRewordsTheWorkingLines) {
    UpdaterController c = MakeController();
    c.setVerificationReinstall(true);
    EXPECT_TRUE(c.state().verification_reinstall);

    c.onStepStarted(UpStep::Download);
    EXPECT_TRUE(c.state().status_line.contains(QStringLiteral("again")))
        << "a same-version run must not claim to download an update";

    c.onStepStarted(UpStep::Install);
    EXPECT_TRUE(c.state().status_line.contains(QStringLiteral("Reinstalling version")));
}

TEST(UpdaterController, VerifyReinstallMismatchIsRedAndOffersNoRetry) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::VerifyReinstallMismatch, QStringLiteral("0.9.1"));
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.steps[size_t(UpStep::Download)], StepStatus::Failed);
    EXPECT_EQ(s.primary_action, QStringLiteral("Close"));
    EXPECT_TRUE(s.secondary_action.isEmpty()) << "re-fetching the same manifest cannot help";
    EXPECT_TRUE(s.detail_text.contains(QStringLiteral("0.9.1")));
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("Nothing was installed")));
}

TEST(UpdaterController, RestoreFailureDoesNotClaimTheOldVersionIsReady) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::RestoreFailed, QStringLiteral("C:/technical/path"));
    const UpdaterUiState& s = c.state();

    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.steps[size_t(UpStep::Verify)], StepStatus::Failed);
    EXPECT_TRUE(s.headline.contains(QStringLiteral("restore")));
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("backup folder")));
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("isn't ready to run")));
    EXPECT_FALSE(s.detail_text.contains(QStringLiteral("C:/technical/path")))
        << "filesystem details belong in logs, not primary UI copy";
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Close"));
}

// ---------------------------------------------------------------------------
// Flow state -- the machine-readable half of the same events
// ---------------------------------------------------------------------------

using exosnap::update::InstallState;
using exosnap::update::UpdatePhase;
using exosnap::update::UpdaterMode;

TEST(UpdaterFlowState, EachRunningStepPublishesItsOwnPhase) {
    UpdaterController c = MakeController();
    c.onStepStarted(UpStep::Download);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Downloading);
    c.onStepStarted(UpStep::CloseApp);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::WaitingForParent);
    c.onStepStarted(UpStep::Install);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Applying);
    c.onStepStarted(UpStep::Verify);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Verifying);
    c.onStepStarted(UpStep::Launch);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Launching);
    c.onAllDone();
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Completed);
}

TEST(UpdaterFlowState, DownloadProgressIsPublishedInBytes) {
    UpdaterController c = MakeController();
    c.onStepStarted(UpStep::Download);
    c.onDownloadProgress(1024, 4096);
    EXPECT_EQ(c.flowState().downloaded_bytes, 1024u);
    EXPECT_EQ(c.flowState().total_bytes, 4096u);
    // An unknown total is reported as 0 rather than guessed, and it must not
    // erase the byte count that IS known.
    c.onDownloadProgress(2048, 0);
    EXPECT_EQ(c.flowState().downloaded_bytes, 2048u);
    EXPECT_EQ(c.flowState().total_bytes, 0u);
}

TEST(UpdaterFlowState, AFailurePublishesCaseRetryEntryAndInstallState) {
    UpdaterController c = MakeController();
    c.onStepStarted(UpStep::Install);
    c.onFailure(FailureCase::VerifyInstallFailed, QString());

    const auto& flow = c.flowState();
    EXPECT_EQ(flow.phase, UpdatePhase::Failed);
    ASSERT_TRUE(flow.failure_case.has_value());
    EXPECT_EQ(*flow.failure_case, FailureCase::VerifyInstallFailed);
    ASSERT_TRUE(flow.retry_entry_step.has_value());
    EXPECT_EQ(*flow.retry_entry_step, UpStep::Install);
    EXPECT_EQ(flow.install_state, InstallState::Restored);
    EXPECT_FALSE(flow.reboot_required);
}

TEST(UpdaterFlowState, AFailureWithNoRetryOnOfferPublishesNoRetryEntry) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::MsiFailed, QStringLiteral("1603"));
    EXPECT_FALSE(c.flowState().retry_entry_step.has_value())
        << "the card offers only Close, so availableActions must not offer a retry";
    EXPECT_EQ(c.state().primary_action, QStringLiteral("Close"));
}

TEST(UpdaterFlowState, TheTwoTerminalSuccessesAreNotReportedAsFailed) {
    UpdaterController reboot = MakeController();
    reboot.onFailure(FailureCase::MsiRebootRequired, QString());
    EXPECT_EQ(reboot.flowState().phase, UpdatePhase::RebootRequired);
    EXPECT_TRUE(reboot.flowState().reboot_required);
    EXPECT_EQ(reboot.flowState().install_state, InstallState::Intact);

    UpdaterController relaunch = MakeController();
    relaunch.onFailure(FailureCase::LaunchFailed, QString());
    EXPECT_EQ(relaunch.flowState().phase, UpdatePhase::RestartPending);
    EXPECT_FALSE(relaunch.flowState().reboot_required)
        << "an app relaunch is not a Windows restart and must not be reported as one";
}

TEST(UpdaterFlowState, TheMsiVerifyFailureDoesNotClaimAnIntactInstall) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::VerifyInstallFailedMsi, QString());
    EXPECT_EQ(c.flowState().install_state, InstallState::Unknown);
    EXPECT_TRUE(c.state().safety_text.contains(QStringLiteral("no rollback is being claimed")));
}

TEST(UpdaterFlowState, RestartingWorkClearsThePreviousFailure) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::DownloadFailed, QString());
    ASSERT_TRUE(c.flowState().failure_case.has_value());
    c.onStepStarted(UpStep::Download);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Downloading);
    EXPECT_FALSE(c.flowState().failure_case.has_value())
        << "a retry in flight must not publish the previous attempt's failure";
    EXPECT_FALSE(c.flowState().retry_entry_step.has_value());
}

// ---------------------------------------------------------------------------
// Manual mode
// ---------------------------------------------------------------------------

UpdaterController MakeManualController() {
    UpdaterController c(QStringLiteral("0.9.0"), QString());
    c.setMode(UpdaterMode::Manual);
    return c;
}

TEST(UpdaterManualFlow, IdleOffersACheckAndNothingElse) {
    UpdaterController c = MakeManualController();
    c.onIdle();
    EXPECT_EQ(c.flowState().mode, UpdaterMode::Manual);
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Idle);
    EXPECT_EQ(c.state().prompt, PromptKind::Idle);
    EXPECT_EQ(c.state().primary_action, QStringLiteral("Check for updates"));
    EXPECT_TRUE(c.state().to_version.isEmpty()) << "no release has been resolved, so there is no target to show";
    for (StepStatus st : c.state().steps)
        EXPECT_EQ(st, StepStatus::Queued);
}

TEST(UpdaterManualFlow, UpToDateIsAResultNotAFailure) {
    UpdaterController c = MakeManualController();
    c.onIdle();
    c.onCheckStarted();
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Checking);
    c.onUpToDate();

    EXPECT_EQ(c.flowState().phase, UpdatePhase::UpToDate);
    EXPECT_FALSE(c.flowState().failure_case.has_value())
        << "\"nothing newer\" used to be reported as a download failure";
    EXPECT_EQ(c.state().variant, TerminalVariant::None);
    EXPECT_EQ(c.state().prompt, PromptKind::UpToDate);
    EXPECT_TRUE(c.state().headline.contains(QStringLiteral("up to date")));
}

TEST(UpdaterManualFlow, AnAvailableUpdateWaitsForAConfirmation) {
    UpdaterController c = MakeManualController();
    c.onIdle();
    c.onCheckStarted();
    c.onUpdateAvailable(QStringLiteral("0.9.1"));

    EXPECT_EQ(c.flowState().phase, UpdatePhase::UpdateAvailable);
    EXPECT_EQ(c.flowState().target_version, std::string("0.9.1"));
    EXPECT_EQ(c.state().to_version, QStringLiteral("0.9.1"));
    EXPECT_EQ(c.state().primary_action, QStringLiteral("Download update"));
    for (StepStatus st : c.state().steps)
        EXPECT_EQ(st, StepStatus::Queued) << "nothing has been fetched yet";
}

TEST(UpdaterManualFlow, ReadyToApplyHaltsBeforeTouchingTheInstallation) {
    UpdaterController c = MakeManualController();
    c.onIdle();
    c.onCheckStarted();
    c.onUpdateAvailable(QStringLiteral("0.9.1"));
    c.onStepStarted(UpStep::Download);
    c.onStepDone(UpStep::Download);
    c.onReadyToApply();

    EXPECT_EQ(c.flowState().phase, UpdatePhase::ReadyToApply);
    EXPECT_EQ(c.state().primary_action, QStringLiteral("Install now"));
    EXPECT_EQ(c.state().steps[size_t(UpStep::Download)], StepStatus::Done);
    EXPECT_EQ(c.state().steps[size_t(UpStep::Install)], StepStatus::Queued);
}

TEST(UpdaterManualFlow, ABlockedCheckReturnsToIdleWithoutClaimingAFailure) {
    UpdaterController c = MakeManualController();
    c.onIdle();
    c.onCheckStarted();
    c.onCheckBlocked(QStringLiteral("This build does not check for updates."));

    EXPECT_EQ(c.flowState().phase, UpdatePhase::Idle);
    EXPECT_FALSE(c.flowState().failure_case.has_value());
    EXPECT_EQ(c.state().prompt, PromptKind::Idle);
    EXPECT_TRUE(c.state().detail_text.contains(QStringLiteral("does not check")));
}

TEST(UpdaterManualFlow, HandoffModeIsNotManual) {
    UpdaterController c = MakeController();
    c.setMode(UpdaterMode::AppHandoff);
    EXPECT_EQ(c.flowState().mode, UpdaterMode::AppHandoff);
    EXPECT_FALSE(c.state().manual);
}

// The footer and the published state offer the SAME thing. A2 is the one case
// whose answer depends on the mode: a manual run downloaded the manifest itself
// and can fetch it again; a handoff run would re-read the file ExoSnap handed
// over and be refused identically, so it offers Close instead of a button that
// provably cannot work.
TEST(UpdaterDownloadVerification, TheFooterOffersExactlyWhatTheStateOffers) {
    UpdaterController manual = MakeManualController();
    manual.onFailure(FailureCase::VerifyDownloadFailed, QString());
    EXPECT_TRUE(manual.flowState().retry_entry_step.has_value());
    EXPECT_EQ(manual.state().primary_action, QStringLiteral("Re-download"));
    EXPECT_EQ(manual.state().secondary_action, QStringLiteral("Close"));

    UpdaterController handoff = MakeController();
    handoff.setMode(UpdaterMode::AppHandoff);
    handoff.onFailure(FailureCase::VerifyDownloadFailed, QString());
    EXPECT_FALSE(handoff.flowState().retry_entry_step.has_value());
    EXPECT_EQ(handoff.state().primary_action, QStringLiteral("Close"));
    EXPECT_TRUE(handoff.state().secondary_action.isEmpty());
    // The security stop keeps its name and its tone either way -- only the
    // affordance changes.
    EXPECT_EQ(handoff.flowState().failure_case, FailureCase::VerifyDownloadFailed);
    EXPECT_EQ(handoff.state().variant, TerminalVariant::Red);
    EXPECT_EQ(handoff.flowState().install_state, InstallState::Intact);
}

TEST(UpdaterTransaction, IsCarriedInThePublishedStateAndSurvivesEveryEvent) {
    UpdaterController c = MakeController();
    c.setMode(UpdaterMode::AppHandoff);
    c.setUpdateTransactionId(QStringLiteral("u-0123456789abcdef"));
    EXPECT_EQ(c.flowState().update_transaction_id, "u-0123456789abcdef");

    // The operation does not change identity because a step did. A runner that
    // correlated once must still be correlating at the terminal state.
    c.onStepStarted(UpStep::Download);
    c.onStepDone(UpStep::Download);
    c.onFailure(FailureCase::VerifyDownloadFailed, QString());
    EXPECT_EQ(c.flowState().update_transaction_id, "u-0123456789abcdef");
}

// A0. The document could not be accepted, so the pipeline was never entered:
// nothing to retry, nothing touched, and copy that points at the app rather than
// at a button this window does not have.
TEST(UpdaterHandoffRejection, IsTerminalOffersNoRetryAndClaimsNothingWasInstalled) {
    UpdaterController c = MakeController();
    c.setMode(UpdaterMode::AppHandoff);
    c.onFailure(FailureCase::HandoffRejected, QStringLiteral("unsupportedVersion (C:/x): handoffVersion 2"));

    EXPECT_EQ(c.flowState().phase, UpdatePhase::Failed);
    EXPECT_TRUE(c.flowState().terminal());
    EXPECT_EQ(c.flowState().failure_case, FailureCase::HandoffRejected);
    EXPECT_FALSE(c.flowState().retry_entry_step.has_value());
    EXPECT_EQ(c.flowState().install_state, InstallState::Intact);

    EXPECT_EQ(c.state().variant, TerminalVariant::Red);
    EXPECT_EQ(c.state().primary_action, QStringLiteral("Close"));
    EXPECT_TRUE(c.state().secondary_action.isEmpty());
    EXPECT_FALSE(c.state().safety_text.isEmpty()) << "a refusal must still state what happened to the installation";
    // No step is marked failed before Download: nothing ran.
    EXPECT_EQ(c.state().steps[size_t(UpStep::CloseApp)], StepStatus::Queued);
}

TEST(UpdaterCancel, IsTerminalAndCarriesNoFailure) {
    // The whole point: a cancellation is neither a success nor a failure. A
    // download cancelled by the user used to surface as FailureCase::
    // DownloadFailed, which describes a network fault that did not happen.
    UpdaterController c = MakeManualController();
    c.onIdle();
    c.onUpdateAvailable(QStringLiteral("0.9.1"));
    c.onStepStarted(UpStep::Download);
    ASSERT_EQ(c.flowState().phase, UpdatePhase::Downloading);

    c.onCancelled();

    EXPECT_EQ(c.flowState().phase, UpdatePhase::Cancelled);
    EXPECT_TRUE(c.flowState().terminal());
    EXPECT_FALSE(c.flowState().failure_case.has_value());
    EXPECT_FALSE(c.flowState().retry_entry_step.has_value());
    EXPECT_EQ(c.flowState().install_state, InstallState::Intact);
    EXPECT_FALSE(c.flowState().reboot_required);
}

TEST(UpdaterCancel, TheStoppedStepIsNotMarkedFailed) {
    // A red cross in the checklist would say the step broke. It was stopped.
    UpdaterController c = MakeManualController();
    c.onStepStarted(UpStep::Download);
    c.onCancelled();
    EXPECT_EQ(c.state().steps[size_t(UpStep::Download)], StepStatus::Queued);
    EXPECT_EQ(c.state().variant, TerminalVariant::None) << "no amber/red terminal card for a cancellation";
    EXPECT_EQ(c.state().prompt, PromptKind::Cancelled);
}

TEST(UpdaterCancel, AManualRunCanStartOverAndAHandoffOnlyCloses) {
    UpdaterController manual = MakeManualController();
    manual.onStepStarted(UpStep::Download);
    manual.onCancelled();
    EXPECT_EQ(manual.state().primary_action, QStringLiteral("Check for updates"));
    EXPECT_EQ(manual.state().secondary_action, QStringLiteral("Close"));

    UpdaterController handoff = MakeController();
    handoff.setMode(UpdaterMode::AppHandoff);
    handoff.onStepStarted(UpStep::Download);
    handoff.onCancelled();
    EXPECT_EQ(handoff.state().primary_action, QStringLiteral("Close"))
        << "the confirmation that started a handoff was given in the app; there is nothing to restart here";
    EXPECT_TRUE(handoff.state().secondary_action.isEmpty());
}

TEST(UpdaterCancel, AFreshCheckLeavesTheCancelledStateBehind) {
    UpdaterController c = MakeManualController();
    c.onStepStarted(UpStep::Download);
    c.onCancelled();
    ASSERT_EQ(c.flowState().phase, UpdatePhase::Cancelled);
    c.onCheckStarted();
    EXPECT_EQ(c.flowState().phase, UpdatePhase::Checking);
    EXPECT_EQ(c.state().prompt, PromptKind::None);
}

// ---------------------------------------------------------------------------
// The pinned-target failure
// ---------------------------------------------------------------------------

TEST(UpdaterController, TargetVersionMismatchNamesBothVersionsAndInstallsNothing) {
    UpdaterController c(QStringLiteral("0.9.0"), QStringLiteral("0.9.1"));
    c.onStepStarted(UpStep::Download);
    c.onFailure(FailureCase::TargetVersionMismatch, QStringLiteral("0.9.2"));
    const UpdaterUiState& s = c.state();

    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.steps[size_t(UpStep::Download)], StepStatus::Failed);
    EXPECT_TRUE(s.detail_text.contains(QStringLiteral("0.9.1"))) << "the version the user was offered";
    EXPECT_TRUE(s.detail_text.contains(QStringLiteral("0.9.2"))) << "the version the feed now serves";
    EXPECT_TRUE(s.safety_text.contains(QStringLiteral("Nothing was installed")));
    EXPECT_EQ(s.primary_action, QStringLiteral("Close"));
    EXPECT_EQ(c.flowState().install_state, InstallState::Intact);
}

} // namespace
