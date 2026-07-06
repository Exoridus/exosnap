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
enum class TerminalVariant : uint8_t { None, Success, Amber, Red, Green };

enum class FailureCase : uint8_t { // brief matrix cases
    DownloadFailed,                // A1  -> Amber
    VerifyDownloadFailed,          // A2  -> Red (security stop)
    AppWontClose,                  // B1  -> Amber
    InstallFailed,                 // B2  -> Amber
    VerifyInstallFailed,           // B3  -> Red (restored)
    LaunchFailed,                  // B4  -> Green (soft success)
    UacDeclined,                   // C1  -> Amber
    MsiFailed,                     // C2  -> Red
};

struct UpdaterUiState {
    std::array<StepStatus, size_t(UpStep::Count)> steps{};
    double ring = 0.0;   // 0..1
    QString status_line; // mono line under the ring
    TerminalVariant variant = TerminalVariant::None;
    QString footer_text;
    QString primary_action;   // "" = hidden
    QString secondary_action; // "" = hidden
    QString from_version, to_version;
};

class UpdaterController {
public:
    UpdaterController(QString from_version, QString to_version);

    void onStepStarted(UpStep s);                        // marks Working + status line
    void onDownloadProgress(quint64 got, quint64 total); // ring within the download band
    void onStepDone(UpStep s);                           // Done + ring snaps to step weight
    void onAllDone();                                    // variant = Success
    void onFailure(FailureCase c, const QString& detail); // maps per matrix

    [[nodiscard]] const UpdaterUiState& state() const { return state_; }

private:
    UpdaterUiState state_;
};
