#pragma once

// update_flow_state.h -- the updater's own state, as one flat value type.
//
// No Qt, no WinAPI, no I/O. This is the shared vocabulary between the process
// that performs an update and anything that observes one: the updater window
// renders from it, the automation channel publishes it, and the tests assert on
// it. There is deliberately no second "automation state" beside it -- a channel
// with its own state model is a channel that can disagree with the product.
//
// Being a plain value with an equality operator is what makes a revision counter
// over it honest: it changes exactly when something observable changed, rather
// than whenever a signal happened to fire.

#include <cstdint>
#include <optional>
#include <string>

#include <update/update_types.h>

namespace exosnap::update {

// ---------------------------------------------------------------------------
// Pipeline vocabulary
// ---------------------------------------------------------------------------

// The five pipeline steps, in order. Kept here rather than in the updater app
// because RetryEntryStep is part of the published state: an automated recovery
// check has to be able to say WHICH step a retry would re-enter.
enum class UpStep : int { Download = 0, CloseApp, Install, Verify, Launch, Count };

enum class StepStatus : std::uint8_t { Queued, Working, Done, Failed };

// The closed failure matrix. Every abort emits exactly one of these, and each
// one maps to a retry entry point (RetryEntryStep) and to a provable statement
// about which installation is live (InstallStateForFailure).
enum class FailureCase : std::uint8_t {
    DownloadFailed,          // A1     -> Amber
    VerifyDownloadFailed,    // A2     -> Red (security stop)
    VerifyReinstallMismatch, // A3     -> Red (verification reinstall gate; nothing installed)
    TargetVersionMismatch,   // A4     -> Red (pinned target gate; nothing installed)
    AppWontClose,            // B1     -> Amber
    InstallFailed,           // B2     -> Amber
    VerifyInstallFailed,     // B3     -> Red (portable: previous version restored)
    RestoreFailed,           // B3-R   -> Red (backup preserved, restore incomplete)
    VerifyInstallFailedMsi,  // B3-MSI -> Red (post-install state could not be confirmed)
    LaunchFailed,            // B4     -> Green (soft success)
    UacDeclined,             // C1     -> Amber
    MsiFailed,               // C2     -> Red
    MsiRebootRequired,       // C3     -> RebootRequired (terminal success; restart pending)
};

// ---------------------------------------------------------------------------
// Flow state
// ---------------------------------------------------------------------------

enum class UpdaterMode : std::uint8_t {
    Manual,       // started by a person; nothing happens without a confirmation
    LegacyHandoff // started by ExoSnap with context arguments; runs the pipeline
};

enum class UpdatePhase : std::uint8_t {
    Idle,             // manual mode, resting: nothing has been asked for yet
    Checking,         // resolving the channel
    UpToDate,         // terminal for a manual check: no newer release on offer
    UpdateAvailable,  // a release was found and is waiting for a confirmation
    Downloading,      // fetching manifest, signature and package
    ReadyToApply,     // verified and staged; waiting for the apply confirmation
    WaitingForParent, // the running ExoSnap has been asked to close
    Applying,         // staged rename, or the elevated msiexec
    Verifying,        // checking the installed files against the signed release
    Launching,        // starting the new version and waiting for it to come up
    RestartPending,   // installed and verified; the app relaunch is outstanding
    RebootRequired,   // terminal success; Windows must restart to finish (C3)
    Completed,        // terminal success; the new version is up
    Failed,           // terminal failure; failure_case says which one
    // Terminal, and deliberately NEITHER of the two above: the operation stopped
    // because it was asked to. Nothing was installed and nothing broke, so
    // reporting it as a failure would send a runner (and a user) looking for a
    // fault that does not exist. It carries no failureCase for the same reason.
    Cancelled,
};

// Which installation is live right now. The single most valuable assertion an
// automated update test can make -- "the existing installation is unharmed" --
// and the reason `failed` alone is not an acceptable answer after an abort.
//
// Only states the updater can actually PROVE. Unknown is not a gap in the model:
// after a Windows Installer run that reported success and then failed
// verification, this process has read a registry path and a version string and
// has NOT asked Windows Installer for a rollback outcome. Claiming `intact`
// there would be a guess, and the whole point of publishing this field is that
// it is not one.
enum class InstallState : std::uint8_t {
    Intact,           // a runnable installation is in place (old or new)
    Restored,         // the swap was undone; the previous version is live again
    StrandedInBackup, // the previous version exists only in the backup directory
    Unknown,          // this process cannot prove which version is live
};

struct UpdateFlowState {
    UpdaterMode mode = UpdaterMode::Manual;
    UpdatePhase phase = UpdatePhase::Idle;
    // Which installation this run is operating on. Published because the two
    // apply paths behave differently in ways an automated check has to know
    // about: the portable swap is unelevated and reversible, the MSI path hands
    // off to an elevated msiexec that this process only observes.
    InstallMode install_mode = InstallMode::Portable;
    // Whether this build may check the update feed at all. False for an
    // unofficial build with no feed override -- a policy refusal, not a failure,
    // and the difference is what stops a dev build from polling the real feed.
    bool checks_enabled = false;

    // Set only in the Failed / RestartPending / RebootRequired phases -- the
    // three terminal outcomes the failure matrix produces.
    std::optional<FailureCase> failure_case;
    // Which step a retry would re-enter. Absent when there is nothing to retry.
    std::optional<UpStep> retry_entry_step;
    InstallState install_state = InstallState::Intact;

    // The installed version, and the version this run is pinned to (empty until
    // a release has been resolved or a target was handed over).
    std::string current_version;
    std::string target_version;

    std::uint64_t downloaded_bytes = 0;
    std::uint64_t total_bytes = 0;

    // A WINDOWS restart, not the app relaunch. Deliberately separate from
    // RestartPending: they call for different actions from whoever is watching.
    bool reboot_required = false;

    [[nodiscard]] bool terminal() const noexcept {
        return phase == UpdatePhase::Completed || phase == UpdatePhase::Failed ||
               phase == UpdatePhase::RebootRequired || phase == UpdatePhase::RestartPending ||
               phase == UpdatePhase::UpToDate || phase == UpdatePhase::Cancelled;
    }

    [[nodiscard]] bool operator==(const UpdateFlowState&) const = default;
};

// ---------------------------------------------------------------------------
// Names and mappings -- one table each, so no consumer writes its own
// ---------------------------------------------------------------------------

[[nodiscard]] const char* UpStepName(UpStep step) noexcept;
[[nodiscard]] const char* UpdaterModeName(UpdaterMode mode) noexcept;
[[nodiscard]] const char* UpdatePhaseName(UpdatePhase phase) noexcept;
[[nodiscard]] const char* FailureCaseName(FailureCase failure) noexcept;
[[nodiscard]] const char* InstallStateName(InstallState state) noexcept;

// Which installation is live after a given failure. A pure table: the two
// portable restore outcomes are already distinct failure cases (B3 restored,
// B3-R stranded), so nothing extra has to be measured to answer this.
[[nodiscard]] InstallState InstallStateForFailure(FailureCase failure) noexcept;

// Which pipeline step a Retry re-enters for a failure:
// A1/A2/A3/A4 -> Download, B1 -> CloseApp, B2/B3/C1/C2 -> Install, B4 -> Launch.
[[nodiscard]] UpStep RetryEntryStep(FailureCase failure) noexcept;

// Whether the product OFFERS a retry for this failure -- a different question
// from where one would re-enter. A verification-reinstall or target-version
// mismatch would re-fetch the same manifest and be refused again; a Windows
// Installer failure and a pending reboot are not this process's to retry; and
// after B4 the product's action is "open the installed version", not "retry".
// The updater window's footer and the automation channel's availableActions read
// this one rule.
[[nodiscard]] bool RetryOffered(FailureCase failure) noexcept;

// The terminal phase a failure case ends in. Only three of the twelve are not
// UpdatePhase::Failed, and each for a reason the product already states: C3 is a
// terminal SUCCESS awaiting a Windows restart, and B4 is a terminal success
// whose only missing piece is the app relaunch.
[[nodiscard]] UpdatePhase PhaseForFailure(FailureCase failure) noexcept;

} // namespace exosnap::update
