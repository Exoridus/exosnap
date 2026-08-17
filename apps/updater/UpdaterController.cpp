#include "UpdaterController.h"

#include <algorithm>

namespace {

using exosnap::update::UpdatePhase;

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
    // The version-less phrasings are not a fallback nobody reaches: a run whose
    // release has not been resolved yet (a manual download started straight from
    // a resolved check, an unpinned handoff) genuinely does not know the number,
    // and printing "Downloading update …" with a hole in it is worse than a
    // sentence that never promised one.
    const bool named = !to_version.isEmpty();
    switch (s) {
    case UpStep::Download:
        if (verification_reinstall)
            return named ? QStringLiteral("Downloading version %1 again…").arg(to_version)
                         : QStringLiteral("Downloading this version again…");
        return named ? QStringLiteral("Downloading update %1…").arg(to_version)
                     : QStringLiteral("Downloading the update…");
    case UpStep::CloseApp:
        return QStringLiteral("Waiting for ExoSnap to close…");
    case UpStep::Install:
        if (verification_reinstall)
            return named ? QStringLiteral("Reinstalling version %1…").arg(to_version)
                         : QStringLiteral("Reinstalling this version…");
        return named ? QStringLiteral("Swapping in version %1…").arg(to_version)
                     : QStringLiteral("Swapping in the new version…");
    case UpStep::Verify:
        return QStringLiteral("Checking signatures & file hashes…");
    case UpStep::Launch:
        return named ? QStringLiteral("Starting version %1…").arg(to_version) : QStringLiteral("Starting ExoSnap…");
    case UpStep::Count:
        break;
    }
    return {};
}

// The phase a running step corresponds to. One mapping, so the window's step
// marks and the published phase can never describe different steps.
UpdatePhase PhaseForStep(UpStep s) {
    switch (s) {
    case UpStep::Download:
        return UpdatePhase::Downloading;
    case UpStep::CloseApp:
        return UpdatePhase::WaitingForParent;
    case UpStep::Install:
        return UpdatePhase::Applying;
    case UpStep::Verify:
        return UpdatePhase::Verifying;
    case UpStep::Launch:
        return UpdatePhase::Launching;
    case UpStep::Count:
        break;
    }
    return UpdatePhase::Idle;
}

// The step a failure case belongs to (marked Failed in the step list).
UpStep FailedStepFor(FailureCase c) {
    switch (c) {
    case FailureCase::DownloadFailed:
    case FailureCase::VerifyDownloadFailed:
    case FailureCase::VerifyReinstallMismatch:
    case FailureCase::TargetVersionMismatch:
    case FailureCase::HandoffRejected:
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
    // The pre-flight frame the window opens on, before the worker has reported
    // anything. It used to be the one state with no status line at all, which
    // left a bare spinner glyph under the ring with nothing to label it.
    state_.status_line = QStringLiteral("Preparing update…");
    flow_.current_version = state_.from_version.toStdString();
    flow_.target_version = state_.to_version.toStdString();
}

void UpdaterController::setVerificationReinstall(bool on) {
    state_.verification_reinstall = on;
}

void UpdaterController::setMode(exosnap::update::UpdaterMode mode) {
    flow_.mode = mode;
    state_.manual = mode == exosnap::update::UpdaterMode::Manual;
}

void UpdaterController::setContext(exosnap::update::InstallMode install_mode, bool checks_enabled) {
    flow_.install_mode = install_mode;
    flow_.checks_enabled = checks_enabled;
}

void UpdaterController::setUpdateTransactionId(const QString& id) {
    flow_.update_transaction_id = id.toStdString();
}

void UpdaterController::setPhase(UpdatePhase phase) {
    flow_.phase = phase;
    // A phase that is not a failure outcome carries no failure detail. Clearing
    // it here rather than at each call site is what keeps a retry from
    // publishing the previous attempt's failure alongside its new progress.
    if (phase != UpdatePhase::Failed && phase != UpdatePhase::RebootRequired && phase != UpdatePhase::RestartPending) {
        flow_.failure_case.reset();
        flow_.retry_entry_step.reset();
        flow_.install_state = exosnap::update::InstallState::Intact;
        flow_.reboot_required = false;
    }
}

// ── Manual-mode events ──────────────────────────────────────────────────────

void UpdaterController::onIdle() {
    state_ = UpdaterUiState{};
    state_.from_version = QString::fromStdString(flow_.current_version);
    state_.manual = flow_.mode == exosnap::update::UpdaterMode::Manual;
    state_.prompt = PromptKind::Idle;
    state_.headline = QStringLiteral("Ready to check for updates");
    state_.detail_text = QStringLiteral("Nothing is downloaded or installed until you say so.");
    state_.safety_text = QStringLiteral("Your installed version is unchanged.");
    state_.primary_action = QStringLiteral("Check for updates");
    state_.secondary_action = QStringLiteral("Close");

    flow_.target_version.clear();
    flow_.downloaded_bytes = 0;
    flow_.total_bytes = 0;
    setPhase(UpdatePhase::Idle);
}

void UpdaterController::onCheckStarted() {
    state_.prompt = PromptKind::None;
    state_.variant = TerminalVariant::None;
    state_.determinate = false;
    state_.ring = 0.0;
    state_.status_line = QStringLiteral("Checking for updates…");
    state_.headline.clear();
    state_.detail_text.clear();
    state_.safety_text.clear();
    state_.primary_action.clear();
    state_.secondary_action.clear();
    setPhase(UpdatePhase::Checking);
}

void UpdaterController::onUpToDate() {
    state_.prompt = PromptKind::UpToDate;
    state_.variant = TerminalVariant::None;
    state_.status_line.clear();
    state_.to_version.clear();
    state_.headline = QStringLiteral("ExoSnap is up to date");
    state_.detail_text = QStringLiteral("Version %1 is the newest release on this channel.").arg(state_.from_version);
    state_.safety_text = QStringLiteral("Nothing was downloaded and nothing was changed.");
    state_.primary_action = QStringLiteral("Check again");
    state_.secondary_action = QStringLiteral("Close");
    flow_.target_version.clear();
    setPhase(UpdatePhase::UpToDate);
}

void UpdaterController::onUpdateAvailable(const QString& version) {
    state_.prompt = PromptKind::UpdateAvailable;
    state_.variant = TerminalVariant::None;
    state_.status_line.clear();
    state_.to_version = version;
    state_.headline = QStringLiteral("Version %1 is available").arg(version);
    state_.detail_text = QStringLiteral("The update is downloaded and verified before anything is replaced.");
    state_.safety_text =
        QStringLiteral("Your installed version %1 stays in place until you apply it.").arg(state_.from_version);
    state_.primary_action = QStringLiteral("Download update");
    state_.secondary_action = QStringLiteral("Close");
    flow_.target_version = version.toStdString();
    setPhase(UpdatePhase::UpdateAvailable);
}

void UpdaterController::onCheckBlocked(const QString& reason) {
    state_.prompt = PromptKind::Idle;
    state_.variant = TerminalVariant::None;
    state_.status_line.clear();
    state_.to_version.clear();
    state_.headline = QStringLiteral("Update checks are turned off in this build");
    state_.detail_text = reason;
    state_.safety_text = QStringLiteral("Nothing was contacted, downloaded or changed.");
    state_.primary_action = QStringLiteral("Close");
    state_.secondary_action.clear();
    flow_.target_version.clear();
    setPhase(UpdatePhase::Idle);
}

void UpdaterController::onCancelled() {
    // The step that was running goes back to Queued rather than to Failed: it
    // did not fail, it was stopped, and a red cross in the checklist would say
    // the opposite of what happened.
    for (StepStatus& st : state_.steps) {
        if (st == StepStatus::Working)
            st = StepStatus::Queued;
    }
    state_.prompt = PromptKind::Cancelled;
    state_.variant = TerminalVariant::None;
    state_.status_line.clear();
    state_.determinate = false;
    state_.ring = 0.0;
    state_.headline = QStringLiteral("Update canceled");
    state_.detail_text = QStringLiteral("The download was stopped and its partial files were discarded.");
    state_.safety_text = state_.from_version.isEmpty()
                             ? QStringLiteral("Nothing was installed and nothing was changed.")
                             : QStringLiteral("Your current version %1 is unchanged.").arg(state_.from_version);
    // Manual runs can start over from here; a handoff run has nothing left to
    // offer, because the confirmation that started it was given in the app.
    const bool manual = flow_.mode == exosnap::update::UpdaterMode::Manual;
    state_.primary_action = manual ? QStringLiteral("Check for updates") : QStringLiteral("Close");
    state_.secondary_action = manual ? QStringLiteral("Close") : QString();

    // setPhase clears the failure detail, which is the point: a cancellation
    // carries no failureCase, no retry entry, and installState intact.
    setPhase(UpdatePhase::Cancelled);
}

void UpdaterController::onReadyToApply() {
    state_.prompt = PromptKind::ReadyToApply;
    state_.variant = TerminalVariant::None;
    state_.status_line.clear();
    state_.headline = QStringLiteral("Version %1 is ready to install").arg(state_.to_version);
    state_.detail_text = QStringLiteral("The signed package was downloaded and its hash checked.");
    state_.safety_text = QStringLiteral("ExoSnap will close while the files are replaced.");
    state_.primary_action = QStringLiteral("Install now");
    state_.secondary_action = QStringLiteral("Close");
    setPhase(UpdatePhase::ReadyToApply);
}

// ── Pipeline events ─────────────────────────────────────────────────────────

void UpdaterController::onStepStarted(UpStep s) {
    if (s == UpStep::Count) {
        return;
    }
    state_.prompt = PromptKind::None;
    state_.steps[size_t(s)] = StepStatus::Working;
    state_.status_line = WorkingStatusLine(s, state_.to_version, state_.verification_reinstall);
    setPhase(PhaseForStep(s));
}

void UpdaterController::onDownloadProgress(quint64 got, quint64 total) {
    flow_.downloaded_bytes = got;
    flow_.total_bytes = total;
    if (total == 0) {
        return; // unknown size: keep the ring where it is
    }
    const double fraction = std::clamp(double(got) / double(total), 0.0, 1.0);
    state_.ring = kStepEndWeight[size_t(UpStep::Download)] * fraction;
    state_.determinate = true;
}

void UpdaterController::onStepDone(UpStep s) {
    if (s == UpStep::Count) {
        return;
    }
    state_.steps[size_t(s)] = StepStatus::Done;
    state_.ring = kStepEndWeight[size_t(s)];
    state_.determinate = true;
}

void UpdaterController::onAllDone() {
    for (StepStatus& st : state_.steps) {
        st = StepStatus::Done;
    }
    state_.ring = 1.0;
    state_.determinate = true;
    state_.variant = TerminalVariant::Success;
    state_.prompt = PromptKind::None;
    state_.status_line.clear();
    setPhase(UpdatePhase::Completed);
}

void UpdaterController::onFailure(FailureCase c, const QString& detail) {
    // MsiRebootRequired is a terminal success: its step (Install) actually
    // completed, so it is marked Done, not Failed.
    state_.steps[size_t(FailedStepFor(c))] =
        c == FailureCase::MsiRebootRequired ? StepStatus::Done : StepStatus::Failed;
    state_.status_line.clear();
    state_.prompt = PromptKind::None;

    // The published state first: phase, the exact case, where a retry would
    // re-enter, and -- the assertion that matters most after an abort -- which
    // installation is live. All four come from the shared matrix tables, so the
    // card copy below and the automation payload cannot describe different
    // outcomes.
    flow_.phase = exosnap::update::PhaseForFailure(c);
    flow_.failure_case = c;
    flow_.retry_entry_step =
        exosnap::update::RetryOffered(c, flow_.mode) ? std::optional<UpStep>(RetryEntryStep(c)) : std::nullopt;
    flow_.install_state = exosnap::update::InstallStateForFailure(c);
    flow_.reboot_required = c == FailureCase::MsiRebootRequired;

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
        // The footer offers exactly what the published state offers. In a handoff
        // run a retry would re-read the manifest ExoSnap handed over and be
        // refused identically, so the card says Close and the next attempt starts
        // in ExoSnap -- which is still running, because A2 aborts before the
        // parent is asked to close.
        if (flow_.retry_entry_step.has_value()) {
            state_.primary_action = QStringLiteral("Re-download");
            state_.secondary_action = QStringLiteral("Close");
        } else {
            state_.primary_action = QStringLiteral("Close");
            state_.secondary_action.clear();
        }
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
    case FailureCase::TargetVersionMismatch: // A4 (pinned target gate)
        // The app offered one version and the channel now resolves to another.
        // Installing the other one would make the offer, the What's-new payload
        // and the installed build three different answers, so the run stops
        // before a single package byte is fetched. Retry would re-fetch the same
        // manifest; a fresh check from the app is the way forward.
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("The offered version is no longer what the channel serves");
        state_.detail_text = QStringLiteral("ExoSnap offered version %1, but the signed release now names %2.")
                                 .arg(QString::fromStdString(flow_.target_version),
                                      detail.isEmpty() ? QStringLiteral("another version") : detail);
        state_.safety_text =
            QStringLiteral("Nothing was installed. Your current version %1 is unchanged.").arg(state_.from_version);
        state_.primary_action = QStringLiteral("Close");
        state_.secondary_action.clear();
        break;
    case FailureCase::HandoffRejected: // A0 (the pipeline never started)
        // Nothing was contacted and nothing was touched: the document ExoSnap
        // wrote could not be accepted, so this process refused before doing any
        // work. Retry would re-read the same file and refuse again, so only
        // Close is offered -- the way forward is a fresh update from the app.
        state_.variant = TerminalVariant::Red;
        state_.headline = QStringLiteral("This update couldn't be started");
        state_.detail_text =
            QStringLiteral("ExoSnap handed over an update this updater can't accept. Start the update again "
                           "from ExoSnap.");
        state_.safety_text = state_.from_version.isEmpty()
                                 ? QStringLiteral("Nothing was downloaded and nothing was changed.")
                                 : QStringLiteral("Nothing was installed. Your current version %1 is unchanged.")
                                       .arg(state_.from_version);
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
        // code cannot back up; only "could not confirm" is truthful. The published
        // installState says the same thing: `unknown`, not `intact`.
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
