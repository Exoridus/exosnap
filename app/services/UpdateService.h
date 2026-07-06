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
#include <QStringList>
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

    // Stage the updater (executable + Qt runtime subset) into
    // %LOCALAPPDATA%\ExoSnap\updater\ and launch it. The app keeps running and
    // exits normally once the updater sends WM_CLOSE to the app window when it is
    // ready to perform the swap. This call does NOT wait for the updater.
    //   * Blocked while recording/finalizing (emits updateError, no-op).
    //   * Emits updaterLaunched() on success, or updateError() on any staging /
    //     launch failure (missing runtime file, spawn failure).
    void LaunchUpdater();

    // Notify-only Scoop detection: true when app_dir_path sits under a Scoop
    // "…/scoop/apps/…" tree (case-insensitive, both path separators). Scoop
    // installs are updated via `scoop update exosnap`, not the staged swap.
    [[nodiscard]] static bool IsScoopManagedInstall(const QString& app_dir_path);

    // Handoff: launch the verified installer. User must confirm in UI first.
    void HandoffToInstaller(const QString& installer_path);

    // Snapshot of the current state (for UI initialisation).
    exosnap::update::UpdateState CurrentState() const;

  signals:
    void updateCheckComplete(exosnap::update::UpdateCheckResult result);
    void updateStateChanged(exosnap::update::UpdateState state);
    void packageReadyForInstall(QString installer_path);
    void updaterLaunched();
    void updateError(exosnap::update::VerifyResult result, QString detail);

  private:
    class Impl;
    Impl* impl_;
};

// ---------------------------------------------------------------------------
// Pure, UI-agnostic helpers behind LaunchUpdater() (defined in
// UpdateLaunchPlan.cpp; exposed here so they can be unit-tested headless).
// ---------------------------------------------------------------------------

// Files copied into the staged updater directory, as paths relative to the app
// dir. Mandatory: the updater exe, Qt6Core/Gui/Widgets.dll, and the windows
// platform plugin. All entries must exist at launch time (LaunchUpdater fails
// with a clear updateError otherwise); the styles plugin is copied best-effort
// on top of this list.
[[nodiscard]] QStringList UpdaterStagingFileList();

// The argv (flags only, excluding argv[0]) the app passes to the staged
// updater. Round-trips through ParseUpdaterArgs.
[[nodiscard]] QStringList BuildUpdaterArgs(const exosnap::update::UpdateState& st, const QString& install_dir,
                                           quint32 pid, const QString& current_version);

} // namespace exosnap
