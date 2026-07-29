#include "UpdaterController.h"

#include <algorithm>

namespace {

// Ring weights: the design canon's four-step weights scaled onto the 5 steps. The ring
// value snaps to the END weight of each completed step; within Download it
// scales linearly with byte progress.
constexpr std::array<double, size_t(UpStep::Count)> kStepEndWeight = {
    0.55, // Download
    0.62, // CloseApp
    0.85, // Install
    0.94, // Verify
    1.0,  // Launch
};

QString WorkingStatusLine(UpStep s, const QString& to_version, bool verification_reinstall) {
    switch (s) {
    case UpStep::Download:
        return verification_reinstall ? QStringLiteral("Downloading version %1 again…").arg(to_version)
                                      : QStringLiteral("Downloading update %1…").arg(to_version);
    case UpStep::CloseApp:
        return QStringLiteral("Waiting for ExoSnap to close…");
    case UpStep::Install:
        return verification_reinstall ? QStringLiteral("Reinstalling version %1…").arg(to_version)
                                      : QStringLiteral("Swapping in version %1…").arg(to_version);
    case UpStep::Verify:
        return QStringLiteral("Checking signatures & file hashes…");
    case UpStep::Launch:
        return QStringLiteral("Starting version %1…").arg(to_version);
    case UpStep::Count:
        break;
    }
    return {};
}

// The step a failure case belongs to (marked Failed in the step list).
UpStep FailedStepFor(FailureCase c) {
    switch (c) {
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::VerifyReinstallMismatch:
        return UpStep::Download;
    case FailureCase::AppWontClose:
        return UpStep::CloseApp;
    case FailureCase::InstallFailed:
    case FailureCase::UacDeclined:
    case FailureCase::MsiFailed:
    case FailureCase::MsiRebootRequired: // succeeded at Install; restart pending
        return UpStep::Install;
    case FailureCase::VerifyInstallFailed:
    case FailureCase::RestoreFailed:
    case FailureCase::VerifyInstallFailedMsi:
        return UpStep::Verify;
    case FailureCase::LaunchFailed:
        return UpStep::Launch;
    }
    return UpStep::Download;
}

} // namespace

UpdaterController::UpdaterController(QString from_version, QString to_version) {
    state_.from_version = std::move(from_version);
    state_.to_version = std::move(to_version);
}

void UpdaterController::setVerificationReinstall(bool on) {
    state_.verification_reinstall = on;
}

void UpdaterController::onStepStarted(UpStep s) {
    if (s == UpStep::Count) {
        return;
    }
    state_.steps[size_t(s)] = StepStatus::Working;
    state_.status_line = WorkingStatusLine(s, state_.to_version, state_.verification_reinstall);
}

void UpdaterController::onDownloadProgress(quint64 got, quint64 total) {
    if (total == 0) {
        return; // unknown size: keep the ring where it is
    }
    const double fraction = std::clamp(double(got) / double(total), 0.0, 1.0);
    state_.ring = kStepEndWeight[size_t(UpStep::Download)] * fraction;
}

void UpdaterController::onStepDone(UpStep s) {
    if (s == UpStep::Count) {
        return;
    }
    state_.steps[size_t(s)] = StepStatus::Done;
    state_.ring = kStepEndWeight[size_t(s)];
}

void UpdaterController::onAllDone() {
    for (StepStatus& st : state_.steps) {
        st = StepStatus::Done;
    }
    state_.ring = 1.0;
    state_.variant = TerminalVariant::Success;
    state_.status_line.clear();
}

void UpdaterController::onFailure(FailureCase c, const QString& detail) {
    // MsiRebootRequired is a terminal success: its step (Install) actually
    // completed, so it is marked Done, not Failed.
    state_.steps[size_t(FailedStepFor(c))] =
        c == FailureCase::MsiRebootRequired ? StepStatus::Done : StepStatus::Failed;
    state_.status_line.clear();

    // Raw WinHTTP/filesystem details stay in stderr evidence (main.cpp); this
    // state carries only actionable user copy and the exact safe-version truth.
    switch (c) {
    case FailureCase::DownloadFailed: // A1
        state_.variant = TerminalVariant::Amber;
        state_.headline = QStringLiteral("Couldn't download the update");
        state_.detail_text = QStringLiteral("Check your internet connection and try again.");
        state_.safety_text =
            QStringLiteral("Your current version %1 is unchanged and still works.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::VerifyDownloadFailed: // A2 (security stop)
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("Download verification failed");
        state_.detail_text =
            QStringLiteral("The downloaded files didn't match the signed release, so they were discarded.");
        state_.safety_text =
            QStringLiteral("Nothing was installed. Your current version %1 is unchanged.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Re-download");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::VerifyReinstallMismatch: // A3 (verification reinstall gate)
        // Nothing was downloaded into place and nothing was installed: the
        // offered manifest simply is not the version this run demanded. Retry
        // would re-fetch the same manifest, so only Close is offered.
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("Verification reinstall unavailable");
        state_.detail_text =
            QStringLiteral("This check requires version %1, but the signed release offers %2.")
                .arg(state_.from_version, detail.isEmpty() ? QStringLiteral("another version") : detail);
        state_.safety_text =
            QStringLiteral("Nothing was installed. Your current version %1 is unchanged.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Close");
        state_.secondary_action.clear();
        break;
    case FailureCase::AppWontClose: // B1
        state_.variant = TerminalVariant::Amber;
        state_.headline = QStringLiteral("Couldn't close ExoSnap");
        state_.detail_text = QStringLiteral("Close the running app, then try the handoff again.");
        state_.safety_text =
            QStringLiteral("Your current version %1 is unchanged and still works.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::InstallFailed: // B2
        state_.variant = TerminalVariant::Amber;
        state_.headline = QStringLiteral("Couldn't install the update");
        state_.detail_text = QStringLiteral("The new files couldn't be put in place. You can try again.");
        state_.safety_text =
            QStringLiteral("Your current version %1 is unchanged and still works.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::VerifyInstallFailed: // B3 (portable: staged-rename backup restored)
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("Update verification failed");
        state_.detail_text =
            QStringLiteral("The installed files didn't match the signed release, so the swap was undone.");
        state_.safety_text =
            QStringLiteral("Your previous version %1 was restored and is ready to run.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::RestoreFailed: // B3-R (portable backup is preserved but not live)
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("Couldn't restore the previous version");
        state_.detail_text =
            QStringLiteral("The update failed and the automatic restore couldn't put the backup back in place.");
        state_.safety_text = QStringLiteral("Version %1 is preserved in the backup folder but isn't ready to run.")
                                 .arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::VerifyInstallFailedMsi: // B3-MSI (post-install state could not be confirmed)
        state_.variant = TerminalVariant::Red;
        // Deliberately does not claim a rollback happened: runVerify() only reads the
        // registry install path and checks the installed version afterward — it never
        // queries Windows Installer for an actual rollback/repair outcome. msiexec
        // returned 0 (success), so asserting "rolled back" here would be a guess this
        // code cannot back up; only "could not confirm" is truthful.
        state_.headline = QStringLiteral("Couldn't confirm the installed version");
        state_.detail_text =
            QStringLiteral("Windows Installer finished, but ExoSnap couldn't verify the installed files.");
        state_.safety_text =
            QStringLiteral("Version %1 may still be installed; no rollback is being claimed.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::LaunchFailed: // B4 (soft success)
        state_.variant = TerminalVariant::Green;
        state_.headline = QStringLiteral("Update complete — version %1 is ready").arg(state_.to_version);
        state_.detail_text =
            QStringLiteral("The update was installed and verified, but the automatic restart didn't open.");
        state_.safety_text =
            QStringLiteral("Version %1 is installed and can be started manually.").arg(state_.to_version);
        state_.primary_action = QStringLiteral("Open ExoSnap");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::UacDeclined: // C1
        state_.variant = TerminalVariant::Amber;
        state_.headline = QStringLiteral("Administrator approval was canceled");
        state_.detail_text = QStringLiteral("Approve the Windows prompt when you retry the installation.");
        state_.safety_text =
            QStringLiteral("Your current version %1 is unchanged and still works.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::MsiFailed: // C2
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("Windows Installer couldn't apply the update");
        state_.detail_text = QStringLiteral("Windows Installer stopped with code %1. Try again later.")
                                 .arg(detail.isEmpty() ? QStringLiteral("unknown") : detail);
        state_.safety_text = QStringLiteral("Your previous version %1 is still usable.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Close");
        state_.secondary_action.clear();
        break;
    case FailureCase::MsiRebootRequired: // C3 (terminal success; restart pending)
        state_.variant = TerminalVariant::RebootRequired;
        state_.headline = QStringLiteral("Update installed — restart Windows to finish");
        state_.detail_text = QStringLiteral("Windows Installer needs a system restart to complete the update.");
        state_.safety_text =
            QStringLiteral("Version %1 keeps working until you restart Windows.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Close");
        state_.secondary_action.clear();
        break;
    }
}
