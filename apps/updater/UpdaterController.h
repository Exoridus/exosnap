#pragma once

// UpdaterController.h -- pure step state machine for the updater window.
//
// No QtWidgets, no I/O: this is a plain value-producing controller that later
// tasks drive from the real download/install worker. It is unit-tested headless
// (QString/QStringList from Qt Core are fine; UpdateChannel/InstallMode come
// from the update strand).

#include <array>
#include <cstddef>
#include <cstdint>

#include <QString>
#include <update/update_types.h>

enum class UpStep : int { Download = 0, CloseApp, Install, Verify, Launch, Count };
enum class StepStatus : uint8_t { Queued, Working, Done, Failed };
// RebootRequired is a terminal SUCCESS (the MSI upgrade applied; Windows must be
// restarted to finish), distinct from the Green soft-success (installed + verified,
// only the auto-relaunch didn't happen).
enum class TerminalVariant : uint8_t { None, Success, Amber, Red, Green, RebootRequired };

enum class FailureCase : uint8_t { // failure matrix cases
    DownloadFailed,                // A1     -> Amber
    VerifyDownloadFailed,          // A2     -> Red (security stop)
    VerifyReinstallMismatch,       // A3     -> Red (verification reinstall gate; nothing installed)
    AppWontClose,                  // B1     -> Amber
    InstallFailed,                 // B2     -> Amber
    VerifyInstallFailed,           // B3     -> Red (portable: previous version restored)
    RestoreFailed,                 // B3-R   -> Red (backup preserved, restore incomplete)
    VerifyInstallFailedMsi,        // B3-MSI -> Red (msiexec rolled back to the previous version)
    LaunchFailed,                  // B4     -> Green (soft success)
    UacDeclined,                   // C1     -> Amber
    MsiFailed,                     // C2     -> Red
    MsiRebootRequired,             // C3     -> RebootRequired (terminal success; restart pending)
};

struct UpdaterUiState {
    std::array<StepStatus, size_t(UpStep::Count)> steps{};
    double ring = 0.0;   // 0..1
    QString status_line; // mono line under the ring
    TerminalVariant variant = TerminalVariant::None;
    // Terminal content is structured so the warning card owns its headline,
    // user-oriented explanation, safety truth and actions as one component.
    QString headline;
    QString detail_text;
    QString safety_text;
    QString primary_action;   // "" = hidden
    QString secondary_action; // "" = hidden
    QString from_version, to_version;
    // ADR 0055: this run reinstalls the IDENTICAL version on purpose. The window
    // marks it (title tag) and the working status lines say "reinstall", so the
    // user is never told a version changed when none did.
    bool verification_reinstall = false;
};

class UpdaterController {
  public:
    UpdaterController(QString from_version, QString to_version);

    // Mark this run as a verification reinstall (ADR 0055). Affects wording only.
    void setVerificationReinstall(bool on);

    void onStepStarted(UpStep s);                         // marks Working + status line
    void onDownloadProgress(quint64 got, quint64 total);  // ring within the download band
    void onStepDone(UpStep s);                            // Done + ring snaps to step weight
    void onAllDone();                                     // variant = Success
    void onFailure(FailureCase c, const QString& detail); // maps per matrix

    [[nodiscard]] const UpdaterUiState& state() const {
        return state_;
    }

  private:
    UpdaterUiState state_;
};
