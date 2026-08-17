#pragma once

// UpdaterController.h -- pure state machine for the updater window.
//
// No QtWidgets, no I/O: this is a plain value-producing controller driven from
// the real download/install worker. It is unit-tested headless (QString from Qt
// Core is fine; the flow vocabulary comes from the update strand).
//
// It produces TWO views of the same run and they are not allowed to disagree:
//   * UpdaterUiState -- what the window paints (copy, tone, step marks).
//   * UpdateFlowState -- the flat product state (mode, phase, failure case,
//     retry entry, install state, versions, bytes) the automation channel
//     publishes.
// Both are written by the same event handlers, from the same events. There is
// deliberately no separate automation state object: a second model is a second
// thing that can be wrong.

#include <array>
#include <cstddef>
#include <cstdint>

#include <QString>
#include <update/update_flow_state.h>
#include <update/update_types.h>

// The pipeline vocabulary now lives in the update strand (see
// update_flow_state.h) because it is published state, not a private detail of
// this window. These names keep every existing call site -- and every test --
// spelled the way it always was.
using UpStep = exosnap::update::UpStep;
using StepStatus = exosnap::update::StepStatus;
using FailureCase = exosnap::update::FailureCase;
using exosnap::update::RetryEntryStep;

// RebootRequired is a terminal SUCCESS (the MSI upgrade applied; Windows must be
// restarted to finish), distinct from the Green soft-success (installed +
// verified, only the auto-relaunch didn't happen).
enum class TerminalVariant : std::uint8_t { None, Success, Amber, Red, Green, RebootRequired };

// A manual-mode resting or confirmation state. These are NOT terminal failures
// and NOT work in flight: the updater is waiting for the person in front of it.
// The window renders them through the same card the terminal states use (one
// component, one geometry) rather than growing a second panel type.
enum class PromptKind : std::uint8_t {
    None,            // working or terminal -- the panel describes the pipeline
    Idle,            // manual entry point: nothing has been asked for yet
    UpToDate,        // a check finished and found nothing newer
    UpdateAvailable, // a release was found; the user decides whether to fetch it
    ReadyToApply,    // downloaded and verified; the user decides whether to apply
    Cancelled,       // the run stopped because it was asked to; nothing happened
};

struct UpdaterUiState {
    std::array<StepStatus, size_t(UpStep::Count)> steps{};
    double ring = 0.0; // 0..1
    // False until the run has produced a real measurement (download bytes, or a
    // completed step). The window paints an indeterminate ring while it is
    // false rather than a large "0 percent", which is a number nothing has
    // measured yet.
    bool determinate = false;
    QString status_line; // mono line under the ring
    TerminalVariant variant = TerminalVariant::None;
    PromptKind prompt = PromptKind::None;
    // Terminal content is structured so the warning card owns its headline,
    // user-oriented explanation, safety truth and actions as one component. The
    // prompt states fill the same four fields.
    QString headline;
    QString detail_text;
    QString safety_text;
    QString primary_action;   // "" = hidden
    QString secondary_action; // "" = hidden
    QString from_version, to_version;
    // ADR 0055: this run reinstalls the IDENTICAL version on purpose. The title
    // bar deliberately does NOT change -- it stays the stable "Updater" role
    // label -- so the marking is content-level: the window's eyebrow reads
    // "REINSTALLING EXOSNAP" and the working status lines say "reinstall", so
    // the user is never told a version changed when none did.
    bool verification_reinstall = false;
    // Manual mode says what it is doing rather than what it is updating: the
    // eyebrow reads "EXOSNAP UPDATER" while resting, because "UPDATING EXOSNAP"
    // over an idle window is a claim about something that is not happening.
    bool manual = false;
};

class UpdaterController {
  public:
    UpdaterController(QString from_version, QString to_version);

    // Mark this run as a verification reinstall (ADR 0055). Affects wording only.
    void setVerificationReinstall(bool on);
    // Manual vs. handoff. Set once, at construction time of the run; it decides
    // which commands exist at all and cannot be derived afterwards.
    void setMode(exosnap::update::UpdaterMode mode);
    // The two facts about this run that never change and that the published
    // state has to carry: which installation it operates on, and whether this
    // build may contact the feed at all.
    void setContext(exosnap::update::InstallMode install_mode, bool checks_enabled);

    // --- Manual-mode events --------------------------------------------------
    void onIdle();                                  // resting entry point
    void onCheckStarted();                          // phase = Checking
    void onUpToDate();                              // terminal for a manual check
    void onUpdateAvailable(const QString& version); // a release was resolved
    void onReadyToApply();                          // verified + staged, awaiting apply
    // The product refuses to check at all (an unofficial build with no feed
    // override). Not a failure of an update -- nothing was attempted -- so it
    // returns to Idle with the reason on the card rather than to Failed.
    void onCheckBlocked(const QString& reason);
    // The run stopped because cancellation was requested and observed. Terminal,
    // and neither a success nor a failure: no failureCase, no retry entry, and
    // the installation is provably untouched (cancellation is only honoured
    // before anything is put in place).
    void onCancelled();

    // --- Pipeline events -----------------------------------------------------
    void onStepStarted(UpStep s);                         // marks Working + status line
    void onDownloadProgress(quint64 got, quint64 total);  // ring within the download band
    void onStepDone(UpStep s);                            // Done + ring snaps to step weight
    void onAllDone();                                     // variant = Success
    void onFailure(FailureCase c, const QString& detail); // maps per matrix

    [[nodiscard]] const UpdaterUiState& state() const {
        return state_;
    }
    [[nodiscard]] const exosnap::update::UpdateFlowState& flowState() const {
        return flow_;
    }

  private:
    void setPhase(exosnap::update::UpdatePhase phase);

    UpdaterUiState state_;
    exosnap::update::UpdateFlowState flow_;
};
