#pragma once
// UpdateService.h -- Qt-aware bridge between the update engine and the UI.
//
// This QObject wraps the UI-agnostic libs/update API and re-emits results as
// Qt signals so ConfigPage (or any other observer) can react without polling.
//
// Design rules (CLAUDE.md):
//   - The engine (libs/update) has NO Qt dependency.
//   - This class is the ONLY Qt-side owner; it calls engine functions on a
//     worker from a QThreadPool it owns, and marshals results back to the main
//     thread via QMetaObject::invokeMethod. Owning the pool is what makes the
//     worker joinable at destruction — see the pool's declaration in Impl.
//   - Recording guard is wired to RecordingCoordinator::State().

#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>
#include <update/update_service_interface.h>
#include <update/update_types.h>
#include <vector>

namespace exosnap {
class RecordingCoordinator;
}

namespace exosnap {

// The persisted channel is a string in AppSettingsStore and an enum in the
// engine. One conversion pair, next to the Qt-side owner of the engine, so a
// second frontend cannot land its own spelling of "Preview".
[[nodiscard]] inline exosnap::update::UpdateChannel UpdateChannelFromString(const QString& channel) {
    return channel.compare(QStringLiteral("Preview"), Qt::CaseInsensitive) == 0
               ? exosnap::update::UpdateChannel::Preview
               : exosnap::update::UpdateChannel::Stable;
}

[[nodiscard]] inline QString UpdateChannelToString(exosnap::update::UpdateChannel channel) {
    using exosnap::update::UpdateChannel;
    return channel == UpdateChannel::Preview ? QStringLiteral("Preview") : QStringLiteral("Stable");
}

enum class UpdateHandoffPhase : uint8_t {
    Idle,
    UpdaterRunning,
    ClosingForHandoff,
};

class UpdateService final : public QObject {
    Q_OBJECT

  public:
    explicit UpdateService(RecordingCoordinator* coordinator, QObject* parent = nullptr);
    ~UpdateService() override;

    // Wire (or replace) the RecordingCoordinator backing the recording guard.
    // RecordPage's coordinator is constructed asynchronously — after runtime
    // capability probing completes, well after MainWindow constructs this
    // service — so MainWindow calls this once RecordPage's coordinatorInitialized()
    // signal fires. Safe to call more than once; nullptr is accepted (yields
    // NotBlocked, matching the pre-wiring default and the nullptr-coordinator
    // construction path exercised by tests).
    void SetRecordingCoordinator(RecordingCoordinator* coordinator);

    // Trigger an async update check on a background thread.
    // No-op if a check is already in progress or blocked.
    void RequestUpdateCheck();

    // Current channel (persisted via AppSettingsStore).
    exosnap::update::UpdateChannel Channel() const;
    void SetChannel(exosnap::update::UpdateChannel ch);

    // Verification reinstall (ADR 0055). Set once at startup from the
    // --verify-update-reinstall CLI flag and never persisted: while on, the check
    // additionally offers the byte-identical version and LaunchUpdater hands the
    // updater its matching --verify-reinstall gate. Off is the shipping default.
    void SetVerifyReinstallMode(bool on);
    [[nodiscard]] bool IsVerifyReinstallMode() const;

    // Current block reason (re-queried each time a check is requested).
    exosnap::update::UpdateBlockReason CurrentBlockReason() const;

    // Stage the updater (executable + Qt runtime subset) into
    // %LOCALAPPDATA%\ExoSnap\updater\ and launch it. The app keeps running and
    // exits normally once the updater sends the marked handoff message when it is
    // ready to perform the swap. This call does NOT wait for the updater.
    //   * Blocked while recording/finalizing (emits updateError, no-op).
    //   * Emits updaterLaunched() on success, or updateError() on any staging /
    //     launch failure (missing runtime file, spawn failure).
    void LaunchUpdater();

    // Notify-only Scoop detection (case-insensitive, both path separators): true
    // when app_dir_path sits under a Scoop tree — either the default
    // "…/scoop/apps/…" layout, or a relocated $env:SCOOP root that still uses the
    // "…/apps/<name>/current" junction layout. Scoop installs are updated via
    // `scoop update exosnap`, not the staged swap.
    [[nodiscard]] static bool IsScoopManagedInstall(const QString& app_dir_path);

    // Handoff: launch the verified installer. User must confirm in UI first.
    void HandoffToInstaller(const QString& installer_path);

    // Snapshot of the current state (for UI initialisation).
    exosnap::update::UpdateState CurrentState() const;

    // WHATS-NEW: the gap-aware release notes (versions in (installed, target],
    // newest first) from the most recent completed check. Empty when the last
    // check found no update. No longer drives the Settings card link (that now
    // uses LastAllChannelNotes()); its only remaining consumer is the pending
    // post-update payload persisted by LaunchUpdater.
    std::vector<exosnap::update::ReleaseNote> LastGapNotes() const;

    // WHATS-NEW: the full reference list of release notes for the active channel from
    // the most recent completed check (newest first), independent of whether an update
    // is available. Empty only before the first completed check. Drives the Settings
    // card "See what's new" link (pre-update mode); the post-update auto-show keeps
    // using LastGapNotes().
    std::vector<exosnap::update::ReleaseNote> LastAllChannelNotes() const;

  signals:
    void updateCheckComplete(exosnap::update::UpdateCheckResult result);
    void updateStateChanged(exosnap::update::UpdateState state);
    void packageReadyForInstall(QString installer_path);
    void updaterLaunched();
    // Emitted only while the old app is still alive and can observe the detached
    // process exit. MainWindow uses it to re-arm a card that was showing
    // "Updater running" after failure, cancellation or user-close.
    void updaterExited(qint64 process_id, quint32 exit_code);
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
// updater. Round-trips through ParseUpdaterArgs. `verify_reinstall` adds
// --verify-reinstall (ADR 0055), which makes the updater refuse every version
// but the one named by `current_version`.
[[nodiscard]] QStringList BuildUpdaterArgs(const exosnap::update::UpdateState& st, const QString& install_dir,
                                           quint32 pid, const QString& current_version, bool verify_reinstall = false);

// Resolve the Settings updates-card state string from a completed check. Pure so
// the loop-guard / recovery semantics can be unit-tested headless:
//   * !update_available                        -> "uptodate"
//   * is_scoop                                 -> "scoop"   (notify-only)
//   * verify mode AND available == current     -> "verify-reinstall" (ADR 0055:
//                                                 the offered version IS the
//                                                 running one, on purpose)
//   * available_version == applied_version     -> "pending" (legacy/runtime loop
//                                                 guard from an accepted marked
//                                                 close handoff)
//   * otherwise                                -> "available"
// A fresh process clears persisted legacy stamps. A user-initiated check also
// clears the in-process stamp before checking, so the same still-applicable
// version can resolve to "available" again.
//
// Order notes: Scoop wins over everything offerable — a Scoop tree is never
// touched by the staged swap, verification mode included. The verification
// reinstall outranks the loop guard because the whole point of that mode is to
// re-run the swap for the version already installed; the mode itself is
// non-persistent, so it cannot leave the card stuck.
[[nodiscard]] QString ResolveUpdateCardState(bool update_available, bool is_scoop, const QString& applied_version,
                                             const QString& available_version, bool verify_reinstall_mode = false,
                                             const QString& current_version = QString(),
                                             UpdateHandoffPhase handoff_phase = UpdateHandoffPhase::Idle);

// The applied-version loop guard is committed only with the marked close
// handoff, never when the detached updater merely starts. Verification
// reinstalls persist nothing by design.
[[nodiscard]] QString AppliedVersionForCommittedHandoff(const QString& target_version, bool verification_reinstall);

// A pending handoff is process-local truth. Any new app process starts by
// discarding the old process's stamp; a successful new version also no longer
// needs it because normal update discovery compares against its real version.
[[nodiscard]] QString ReconcileAppliedVersionOnStartup(const QString& persisted_applied_version);

} // namespace exosnap
