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

QString WorkingStatusLine(UpStep s, const QString& to_version) {
    switch (s) {
    case UpStep::Download:
        return QStringLiteral("Downloading update %1…").arg(to_version);
    case UpStep::CloseApp:
        return QStringLiteral("Waiting for ExoSnap to close…");
    case UpStep::Install:
        return QStringLiteral("Swapping in version %1…").arg(to_version);
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
        return UpStep::Download;
    case FailureCase::AppWontClose:
        return UpStep::CloseApp;
    case FailureCase::InstallFailed:
    case FailureCase::UacDeclined:
    case FailureCase::MsiFailed:
        return UpStep::Install;
    case FailureCase::VerifyInstallFailed:
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

void UpdaterController::onStepStarted(UpStep s) {
    if (s == UpStep::Count) {
        return;
    }
    state_.steps[size_t(s)] = StepStatus::Working;
    state_.status_line = WorkingStatusLine(s, state_.to_version);
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
    state_.steps[size_t(FailedStepFor(c))] = StepStatus::Failed;
    state_.status_line.clear();

    // Copy is VERBATIM from the failure brief matrix; %1 = version string
    // (B4: target version) or the installer exit code (C2). — = em dash.
    switch (c) {
    case FailureCase::DownloadFailed: // A1
        state_.variant = TerminalVariant::Amber;
        state_.footer_text = QStringLiteral("Download failed. Your current version is unchanged.");
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::VerifyDownloadFailed: // A2 (security stop)
        state_.variant = TerminalVariant::Red;
        state_.footer_text = QStringLiteral(
            "Verification failed — the download may be corrupt or tampered. "
            "Nothing was installed.");
        state_.primary_action = QStringLiteral("Re-download");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::AppWontClose: // B1
        state_.variant = TerminalVariant::Amber;
        state_.footer_text =
            QStringLiteral("Couldn't close the running ExoSnap. Please close it and retry.");
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::InstallFailed: // B2
        state_.variant = TerminalVariant::Amber;
        state_.footer_text =
            QStringLiteral("Couldn't install the update. Your current version still works.");
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Open current version");
        break;
    case FailureCase::VerifyInstallFailed: // B3 (restored)
        state_.variant = TerminalVariant::Red;
        state_.footer_text = QStringLiteral(
            "Update verification failed — your previous version was restored.");
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Open current version");
        break;
    case FailureCase::LaunchFailed: // B4 (soft success)
        state_.variant = TerminalVariant::Green;
        state_.footer_text = QStringLiteral("Update complete — version %1 is ready. You can "
                                            "close this window and start ExoSnap.")
                                 .arg(state_.to_version);
        state_.primary_action = QStringLiteral("Open ExoSnap");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::UacDeclined: // C1
        state_.variant = TerminalVariant::Amber;
        state_.footer_text = QStringLiteral("The update needs administrator approval. "
                                            "Update canceled; your version is unchanged.");
        state_.primary_action = QStringLiteral("Retry");
        state_.secondary_action = QStringLiteral("Close");
        break;
    case FailureCase::MsiFailed: // C2
        state_.variant = TerminalVariant::Red;
        state_.footer_text =
            QStringLiteral("Installation failed (code %1). Your previous version is still usable.")
                .arg(detail);
        state_.primary_action = QStringLiteral("Close");
        state_.secondary_action.clear();
        break;
    }
}
