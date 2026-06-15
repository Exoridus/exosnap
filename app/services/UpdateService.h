#pragma once
// UpdateService.h -- Qt-aware bridge between the update engine and the UI.
//
// This QObject wraps the UI-agnostic libs/update API and re-emits results as
// Qt signals so ConfigPage (or any other observer) can react without polling.
//
// Design rules (CLAUDE.md):
//   - The engine (libs/update) has NO Qt dependency.
//   - This class is the ONLY Qt-side owner; it calls engine functions on a
//     QThread worker and marshals results back to the main thread via
//     QMetaObject::invokeMethod.
//   - Recording guard is wired to RecordingCoordinator::State().

#include <QObject>
#include <QString>
#include <optional>
#include <update/update_service_interface.h>
#include <update/update_types.h>

namespace exosnap {
class RecordingCoordinator;
}

namespace exosnap {

class UpdateService final : public QObject {
    Q_OBJECT

  public:
    explicit UpdateService(RecordingCoordinator* coordinator, QObject* parent = nullptr);
    ~UpdateService() override;

    // Trigger an async update check on a background thread.
    // No-op if a check is already in progress or blocked.
    void RequestUpdateCheck();

    // Current channel (persisted via AppSettingsStore).
    exosnap::update::UpdateChannel Channel() const;
    void SetChannel(exosnap::update::UpdateChannel ch);

    // Current block reason (re-queried each time a check is requested).
    exosnap::update::UpdateBlockReason CurrentBlockReason() const;

    // Initiate download and verify of the update package.
    // Only valid when install_mode == Installed and an update is available.
    // Emits packageReadyForInstall() or updateError() asynchronously.
    void RequestDownloadAndVerify();

    // Handoff: launch the verified installer. User must confirm in UI first.
    void HandoffToInstaller(const QString& installer_path);

    // Snapshot of the current state (for UI initialisation).
    exosnap::update::UpdateState CurrentState() const;

  signals:
    void updateCheckComplete(exosnap::update::UpdateCheckResult result);
    void updateStateChanged(exosnap::update::UpdateState state);
    void packageReadyForInstall(QString installer_path);
    void updateError(exosnap::update::VerifyResult result, QString detail);

  private:
    class Impl;
    Impl* impl_;
};

} // namespace exosnap
