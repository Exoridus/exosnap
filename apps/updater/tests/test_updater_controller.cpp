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
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("previous version was restored")));
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Open current version"));
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
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("could not be confirmed")));
    EXPECT_FALSE(s.footer_text.contains(QStringLiteral("was restored")));
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Open current version"));
}

TEST(UpdaterController, MsiRebootRequiredIsTerminalSuccessWithRestartCopy) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::MsiRebootRequired, QString());

    const UpdaterUiState& s = c.state();
    // The upgrade applied: the Install step is Done, not Failed, and this is its
    // own terminal variant rather than a red failure card.
    EXPECT_EQ(s.steps[size_t(UpStep::Install)], StepStatus::Done);
    EXPECT_EQ(s.variant, TerminalVariant::RebootRequired);
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("restart Windows")));
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("installed")));
    // Not the generic C2 failure wording.
    EXPECT_FALSE(s.footer_text.contains(QStringLiteral("Installation failed")));
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
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("0.9.0")));
}

TEST(UpdaterController, DownloadFailedIsAmber) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::DownloadFailed, QString());
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Amber);
    EXPECT_EQ(s.steps[size_t(UpStep::Download)], StepStatus::Failed);
    EXPECT_EQ(s.primary_action, QStringLiteral("Retry"));
    EXPECT_EQ(s.secondary_action, QStringLiteral("Close"));
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("current version is unchanged")));
}

TEST(UpdaterController, VerifyDownloadFailedIsRedSecurityStop) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::VerifyDownloadFailed, QString());
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.primary_action, QStringLiteral("Re-download"));
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("corrupt or tampered")));
}

TEST(UpdaterController, MsiFailedEmbedsCodeAndHasNoSecondary) {
    UpdaterController c = MakeController();
    c.onFailure(FailureCase::MsiFailed, QStringLiteral("1603"));
    const UpdaterUiState& s = c.state();
    EXPECT_EQ(s.variant, TerminalVariant::Red);
    EXPECT_EQ(s.primary_action, QStringLiteral("Close"));
    EXPECT_TRUE(s.secondary_action.isEmpty());
    EXPECT_TRUE(s.footer_text.contains(QStringLiteral("1603")));
}

} // namespace
