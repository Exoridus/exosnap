// test_update_flow_state.cpp -- the updater's published state vocabulary.
//
// The tables here are the ones an automated recovery check reads: which step a
// retry re-enters, whether the product offers one at all, which terminal phase a
// failure ends in, and -- the assertion that matters most after an abort --
// which installation is live. Each is covered for EVERY case in the matrix,
// because a table with a hole in it answers wrongly rather than not at all.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <update/update_flow_state.h>

namespace {

using exosnap::update::FailureCase;
using exosnap::update::InstallState;
using exosnap::update::InstallStateForFailure;
using exosnap::update::PhaseForFailure;
using exosnap::update::RetryEntryStep;
using exosnap::update::RetryOffered;
using exosnap::update::UpdateFlowState;
using exosnap::update::UpdatePhase;
using exosnap::update::UpdaterMode;
using exosnap::update::UpStep;

// Every failure case, in matrix order. The tests below iterate this so a new
// case cannot be added without showing up in the totals.
const std::vector<FailureCase>& AllFailures() {
    static const std::vector<FailureCase> cases = {
        FailureCase::DownloadFailed,
        FailureCase::VerifyDownloadFailed,
        FailureCase::VerifyReinstallMismatch,
        FailureCase::TargetVersionMismatch,
        FailureCase::HandoffRejected,
        FailureCase::AppWontClose,
        FailureCase::InstallFailed,
        FailureCase::VerifyInstallFailed,
        FailureCase::RestoreFailed,
        FailureCase::VerifyInstallFailedMsi,
        FailureCase::LaunchFailed,
        FailureCase::UacDeclined,
        FailureCase::MsiFailed,
        FailureCase::MsiRebootRequired,
    };
    return cases;
}

TEST(UpdateFlowState, EveryFailureCaseHasAName) {
    for (const FailureCase failure : AllFailures()) {
        EXPECT_FALSE(std::string(exosnap::update::FailureCaseName(failure)).empty());
    }
}

TEST(UpdateFlowState, PhaseNamesAreDistinct) {
    // A duplicate name would make two different phases indistinguishable on the
    // wire while still comparing unequal in code -- the worst possible split.
    const std::vector<UpdatePhase> phases = {
        UpdatePhase::Idle,
        UpdatePhase::Checking,
        UpdatePhase::UpToDate,
        UpdatePhase::UpdateAvailable,
        UpdatePhase::Downloading,
        UpdatePhase::ReadyToApply,
        UpdatePhase::WaitingForParent,
        UpdatePhase::Applying,
        UpdatePhase::Verifying,
        UpdatePhase::Launching,
        UpdatePhase::RestartPending,
        UpdatePhase::RebootRequired,
        UpdatePhase::Completed,
        UpdatePhase::Failed,
        UpdatePhase::Cancelled,
    };
    std::vector<std::string> names;
    for (const UpdatePhase phase : phases)
        names.emplace_back(exosnap::update::UpdatePhaseName(phase));
    for (size_t i = 0; i < names.size(); ++i) {
        EXPECT_FALSE(names[i].empty());
        for (size_t j = i + 1; j < names.size(); ++j)
            EXPECT_NE(names[i], names[j]) << "phases " << i << " and " << j << " share a name";
    }
}

// -- installState -----------------------------------------------------------

TEST(InstallStateForFailure, NothingWasTouchedBeforeTheSwap) {
    for (const FailureCase failure :
         {FailureCase::DownloadFailed, FailureCase::VerifyDownloadFailed, FailureCase::VerifyReinstallMismatch,
          FailureCase::TargetVersionMismatch, FailureCase::HandoffRejected, FailureCase::AppWontClose,
          FailureCase::InstallFailed, FailureCase::UacDeclined}) {
        EXPECT_EQ(InstallStateForFailure(failure), InstallState::Intact)
            << "case " << exosnap::update::FailureCaseName(failure);
    }
}

TEST(InstallStateForFailure, PortableVerifyFailureReportsTheRestore) {
    // B3 is the case whose whole meaning is "the swap was undone": reporting it
    // as `intact` would be true about runnability and wrong about which version
    // is live, which is the thing a recovery check has to distinguish.
    EXPECT_EQ(InstallStateForFailure(FailureCase::VerifyInstallFailed), InstallState::Restored);
}

TEST(InstallStateForFailure, FailedRestoreIsStrandedNotRestored) {
    EXPECT_EQ(InstallStateForFailure(FailureCase::RestoreFailed), InstallState::StrandedInBackup);
}

TEST(InstallStateForFailure, MsiVerifyFailureRefusesToGuess) {
    // The shipping failure copy says "no rollback is being claimed" because this
    // process never asked Windows Installer for one. The published state has to
    // say the same thing: `unknown`, never `intact`.
    EXPECT_EQ(InstallStateForFailure(FailureCase::VerifyInstallFailedMsi), InstallState::Unknown);
}

TEST(InstallStateForFailure, TerminalSuccessesLeaveARunnableInstall) {
    EXPECT_EQ(InstallStateForFailure(FailureCase::LaunchFailed), InstallState::Intact);
    EXPECT_EQ(InstallStateForFailure(FailureCase::MsiRebootRequired), InstallState::Intact);
}

// -- retry ------------------------------------------------------------------

TEST(RetryEntryStepTable, RoutesEveryCaseToAnExistingStep) {
    for (const FailureCase failure : AllFailures()) {
        const UpStep entry = RetryEntryStep(failure);
        EXPECT_NE(entry, UpStep::Count) << "case " << exosnap::update::FailureCaseName(failure);
    }
}

TEST(RetryEntryStepTable, PinnedTargetMismatchWouldRestartAtDownload) {
    EXPECT_EQ(RetryEntryStep(FailureCase::TargetVersionMismatch), UpStep::Download);
}

// A0. The handoff document is an INPUT to this process: a refused one is not
// something the updater can repair by trying again, and the installation claim
// needs no evidence beyond the ordering -- the pipeline was never entered.
TEST(HandoffRejectedCase, IsATruthfulTerminalFailureThatTouchedNothing) {
    EXPECT_EQ(PhaseForFailure(FailureCase::HandoffRejected), UpdatePhase::Failed);
    EXPECT_EQ(InstallStateForFailure(FailureCase::HandoffRejected), InstallState::Intact);
    EXPECT_FALSE(RetryOffered(FailureCase::HandoffRejected, UpdaterMode::AppHandoff));
    EXPECT_EQ(std::string(exosnap::update::FailureCaseName(FailureCase::HandoffRejected)), "handoffRejected");
}

// The mode is published vocabulary, and it now describes what the run IS rather
// than what it used to be called.
TEST(UpdaterModeNames, NameTheTwoProductModes) {
    EXPECT_EQ(std::string(exosnap::update::UpdaterModeName(exosnap::update::UpdaterMode::Manual)), "manual");
    EXPECT_EQ(std::string(exosnap::update::UpdaterModeName(exosnap::update::UpdaterMode::AppHandoff)), "appHandoff");
}

TEST(RetryOfferedTable, VersionGatesAndTerminalSuccessesOfferNoRetry) {
    // A retry would re-fetch the same manifest and be refused again; a terminal
    // success has nothing to repeat. True in both modes.
    for (const UpdaterMode mode : {UpdaterMode::Manual, UpdaterMode::AppHandoff}) {
        for (const FailureCase failure :
             {FailureCase::VerifyReinstallMismatch, FailureCase::TargetVersionMismatch, FailureCase::HandoffRejected,
              FailureCase::LaunchFailed, FailureCase::MsiFailed, FailureCase::MsiRebootRequired}) {
            EXPECT_FALSE(RetryOffered(failure, mode)) << "case " << exosnap::update::FailureCaseName(failure);
        }
    }
}

TEST(RetryOfferedTable, RecoverableFailuresOfferOne) {
    for (const UpdaterMode mode : {UpdaterMode::Manual, UpdaterMode::AppHandoff}) {
        for (const FailureCase failure :
             {FailureCase::DownloadFailed, FailureCase::AppWontClose, FailureCase::InstallFailed,
              FailureCase::VerifyInstallFailed, FailureCase::RestoreFailed, FailureCase::VerifyInstallFailedMsi,
              FailureCase::UacDeclined}) {
            EXPECT_TRUE(RetryOffered(failure, mode)) << "case " << exosnap::update::FailureCaseName(failure);
        }
    }
}

// A2 is the one case whose answer depends on the mode, and ADR 0068 is why: a
// manual run downloads the manifest itself, so a retry can genuinely fetch it
// again; a handoff run re-reads the exact file the application handed over, so
// the same refusal is guaranteed. Offering nothing is never a false promise --
// offering a button that provably cannot work is.
TEST(RetryOfferedTable, DownloadVerificationRetryDependsOnWhoFetchedTheManifest) {
    EXPECT_TRUE(RetryOffered(FailureCase::VerifyDownloadFailed, UpdaterMode::Manual));
    EXPECT_FALSE(RetryOffered(FailureCase::VerifyDownloadFailed, UpdaterMode::AppHandoff));
}

// -- terminal phase ---------------------------------------------------------

TEST(PhaseForFailureTable, TheTwoTerminalSuccessesAreNotFailed) {
    EXPECT_EQ(PhaseForFailure(FailureCase::MsiRebootRequired), UpdatePhase::RebootRequired);
    EXPECT_EQ(PhaseForFailure(FailureCase::LaunchFailed), UpdatePhase::RestartPending);
}

TEST(PhaseForFailureTable, EveryOtherCaseIsFailed) {
    for (const FailureCase failure : AllFailures()) {
        if (failure == FailureCase::MsiRebootRequired || failure == FailureCase::LaunchFailed)
            continue;
        EXPECT_EQ(PhaseForFailure(failure), UpdatePhase::Failed)
            << "case " << exosnap::update::FailureCaseName(failure);
    }
}

// -- the value type ---------------------------------------------------------

TEST(UpdateFlowStateValue, ComparesFieldByField) {
    // The revision counter that rides on this struct is only honest if equality
    // is structural: a state that differs in one field must not compare equal.
    UpdateFlowState a;
    UpdateFlowState b;
    EXPECT_EQ(a, b);
    b.phase = UpdatePhase::Downloading;
    EXPECT_NE(a, b);
    b = a;
    b.downloaded_bytes = 1;
    EXPECT_NE(a, b);
    b = a;
    b.install_state = InstallState::Restored;
    EXPECT_NE(a, b);
    // The correlation identity is part of the observable state, so a state that
    // belongs to a different operation must not compare equal to this one.
    b = a;
    b.update_transaction_id = "u-0123456789abcdef";
    EXPECT_NE(a, b);
}

TEST(UpdateFlowStateValue, TerminalCoversEveryEndState) {
    UpdateFlowState state;
    for (const UpdatePhase phase : {UpdatePhase::Completed, UpdatePhase::Failed, UpdatePhase::RebootRequired,
                                    UpdatePhase::RestartPending, UpdatePhase::UpToDate, UpdatePhase::Cancelled}) {
        state.phase = phase;
        EXPECT_TRUE(state.terminal()) << exosnap::update::UpdatePhaseName(phase);
    }
    for (const UpdatePhase phase : {UpdatePhase::Idle, UpdatePhase::Checking, UpdatePhase::UpdateAvailable,
                                    UpdatePhase::Downloading, UpdatePhase::ReadyToApply, UpdatePhase::WaitingForParent,
                                    UpdatePhase::Applying, UpdatePhase::Verifying, UpdatePhase::Launching}) {
        state.phase = phase;
        EXPECT_FALSE(state.terminal()) << exosnap::update::UpdatePhaseName(phase);
    }
}

} // namespace
