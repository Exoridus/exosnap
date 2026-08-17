// update_flow_state.cpp -- the name and mapping tables behind the updater's
// published state. Pure switches with no default label, so adding a phase or a
// failure case is a compile error here rather than a silent "unknown" on the
// wire.

#include <update/update_flow_state.h>

namespace exosnap::update {

const char* UpStepName(UpStep step) noexcept {
    switch (step) {
    case UpStep::Download:
        return "download";
    case UpStep::CloseApp:
        return "closeApp";
    case UpStep::Install:
        return "install";
    case UpStep::Verify:
        return "verify";
    case UpStep::Launch:
        return "launch";
    case UpStep::Count:
        break;
    }
    return "";
}

const char* UpdaterModeName(UpdaterMode mode) noexcept {
    switch (mode) {
    case UpdaterMode::Manual:
        return "manual";
    case UpdaterMode::AppHandoff:
        return "appHandoff";
    }
    return "manual";
}

const char* UpdatePhaseName(UpdatePhase phase) noexcept {
    switch (phase) {
    case UpdatePhase::Idle:
        return "idle";
    case UpdatePhase::Checking:
        return "checking";
    case UpdatePhase::UpToDate:
        return "upToDate";
    case UpdatePhase::UpdateAvailable:
        return "updateAvailable";
    case UpdatePhase::Downloading:
        return "downloading";
    case UpdatePhase::ReadyToApply:
        return "readyToApply";
    case UpdatePhase::WaitingForParent:
        return "waitingForParent";
    case UpdatePhase::Applying:
        return "applying";
    case UpdatePhase::Verifying:
        return "verifying";
    case UpdatePhase::Launching:
        return "launching";
    case UpdatePhase::RestartPending:
        return "restartPending";
    case UpdatePhase::RebootRequired:
        return "rebootRequired";
    case UpdatePhase::Completed:
        return "completed";
    case UpdatePhase::Failed:
        return "failed";
    case UpdatePhase::Cancelled:
        return "cancelled";
    }
    return "idle";
}

const char* FailureCaseName(FailureCase failure) noexcept {
    switch (failure) {
    case FailureCase::DownloadFailed:
        return "downloadFailed";
    case FailureCase::VerifyDownloadFailed:
        return "verifyDownloadFailed";
    case FailureCase::VerifyReinstallMismatch:
        return "verifyReinstallMismatch";
    case FailureCase::TargetVersionMismatch:
        return "targetVersionMismatch";
    case FailureCase::HandoffRejected:
        return "handoffRejected";
    case FailureCase::AppWontClose:
        return "appWontClose";
    case FailureCase::InstallFailed:
        return "installFailed";
    case FailureCase::VerifyInstallFailed:
        return "verifyInstallFailed";
    case FailureCase::RestoreFailed:
        return "restoreFailed";
    case FailureCase::VerifyInstallFailedMsi:
        return "verifyInstallFailedMsi";
    case FailureCase::LaunchFailed:
        return "launchFailed";
    case FailureCase::UacDeclined:
        return "uacDeclined";
    case FailureCase::MsiFailed:
        return "msiFailed";
    case FailureCase::MsiRebootRequired:
        return "msiRebootRequired";
    }
    return "downloadFailed";
}

const char* InstallStateName(InstallState state) noexcept {
    switch (state) {
    case InstallState::Intact:
        return "intact";
    case InstallState::Restored:
        return "restored";
    case InstallState::StrandedInBackup:
        return "strandedInBackup";
    case InstallState::Unknown:
        return "unknown";
    }
    return "unknown";
}

InstallState InstallStateForFailure(FailureCase failure) noexcept {
    switch (failure) {
    // Nothing was ever put in place: the run stopped while resolving,
    // downloading, verifying the download or waiting for the parent to close.
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::VerifyReinstallMismatch:
    case FailureCase::TargetVersionMismatch:
    case FailureCase::AppWontClose:
        return InstallState::Intact;
    // A0: the handoff was refused before the pipeline was entered at all, so no
    // file outside this process's own reasoning was ever opened for writing.
    case FailureCase::HandoffRejected:
        return InstallState::Intact;
    // B2: StageRename reports InstallFailed only for its three "nothing touched"
    // errors (staging missing, backup collision, install->backup rename failed).
    case FailureCase::InstallFailed:
        return InstallState::Intact;
    // C1: the elevation was declined before msiexec ran at all.
    case FailureCase::UacDeclined:
        return InstallState::Intact;
    // C2: Windows Installer is transactional and rolls its own failed
    // transaction back; the previous version stays usable. This is the same
    // statement the shipping failure card already makes.
    case FailureCase::MsiFailed:
        return InstallState::Intact;
    // C3: the upgrade DID apply; only the Windows restart is outstanding.
    case FailureCase::MsiRebootRequired:
        return InstallState::Intact;
    // B4: installed and verified. The runnable install is the NEW version --
    // only its automatic start did not happen.
    case FailureCase::LaunchFailed:
        return InstallState::Intact;
    // B3: the swap was undone and the previous version is live again.
    case FailureCase::VerifyInstallFailed:
        return InstallState::Restored;
    // B3-R: the compensating restore itself failed. The old tree exists, in the
    // backup directory, and the install directory does not hold a runnable app.
    case FailureCase::RestoreFailed:
        return InstallState::StrandedInBackup;
    // B3-MSI: msiexec returned success and the version could not be confirmed
    // afterwards. This process never asked Windows Installer for a rollback
    // outcome, so it cannot claim one -- see InstallState::Unknown.
    case FailureCase::VerifyInstallFailedMsi:
        return InstallState::Unknown;
    }
    return InstallState::Unknown;
}

UpStep RetryEntryStep(FailureCase failure) noexcept {
    switch (failure) {
    case FailureCase::DownloadFailed:          // A1
    case FailureCase::VerifyDownloadFailed:    // A2 (file already deleted)
    case FailureCase::VerifyReinstallMismatch: // A3 (nothing downloaded into place)
    case FailureCase::TargetVersionMismatch:   // A4 (nothing downloaded into place)
    case FailureCase::HandoffRejected:         // A0 (the pipeline never started)
        return UpStep::Download;
    case FailureCase::AppWontClose: // B1 (download kept)
        return UpStep::CloseApp;
    case FailureCase::InstallFailed:          // B2 (staging kept)
    case FailureCase::VerifyInstallFailed:    // B3 (portable: previous version restored)
    case FailureCase::RestoreFailed:          // B3-R (backup preserved; retry heals it)
    case FailureCase::VerifyInstallFailedMsi: // B3-MSI
    case FailureCase::UacDeclined:            // C1 (re-handoff)
    case FailureCase::MsiFailed:              // C2
    case FailureCase::MsiRebootRequired:      // C3 (terminal success; no Retry offered)
        return UpStep::Install;
    case FailureCase::LaunchFailed: // B4 (soft success; manual start)
        return UpStep::Launch;
    }
    return UpStep::Download;
}

bool RetryOffered(FailureCase failure) noexcept {
    switch (failure) {
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::AppWontClose:
    case FailureCase::InstallFailed:
    case FailureCase::VerifyInstallFailed:
    case FailureCase::RestoreFailed:
    case FailureCase::VerifyInstallFailedMsi:
    case FailureCase::UacDeclined:
        return true;
    // A3/A4: the same manifest would be fetched and refused again.
    case FailureCase::VerifyReinstallMismatch:
    case FailureCase::TargetVersionMismatch:
    // A0: the handoff document is an input to this process, not something it can
    // repair. Re-reading the same file would be refused for the same reason.
    case FailureCase::HandoffRejected:
    // B4: the update applied; the action is to start the installed version.
    case FailureCase::LaunchFailed:
    // C2/C3: Windows Installer's own outcome; not this process's to repeat.
    case FailureCase::MsiFailed:
    case FailureCase::MsiRebootRequired:
        return false;
    }
    return false;
}

UpdatePhase PhaseForFailure(FailureCase failure) noexcept {
    switch (failure) {
    case FailureCase::MsiRebootRequired:
        return UpdatePhase::RebootRequired;
    case FailureCase::LaunchFailed:
        return UpdatePhase::RestartPending;
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::VerifyReinstallMismatch:
    case FailureCase::TargetVersionMismatch:
    case FailureCase::HandoffRejected:
    case FailureCase::AppWontClose:
    case FailureCase::InstallFailed:
    case FailureCase::VerifyInstallFailed:
    case FailureCase::RestoreFailed:
    case FailureCase::VerifyInstallFailedMsi:
    case FailureCase::UacDeclined:
    case FailureCase::MsiFailed:
        return UpdatePhase::Failed;
    }
    return UpdatePhase::Failed;
}

} // namespace exosnap::update
