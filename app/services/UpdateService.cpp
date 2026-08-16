// UpdateService.cpp -- Qt bridge implementation for the update engine.

#include "UpdateService.h"

#include <update/install_mode_detector.h>
#include <update/manifest_io.h>
#include <update/package_verifier.h>
#include <update/update_checker.h>
#include <update/update_types.h>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion (generated from PROJECT_VERSION)
#include "RecordingCoordinator.h"
#include "UpdateCheckGate.h"
#include "WhatsNewPayload.h"

#include "../diagnostics/AppLog.h"

#include "../viewmodels/RecordViewModel.h" // for UiRecordingState

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QWinEventNotifier>
#include <atomic>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace exosnap {

// ---------------------------------------------------------------------------
// Internal implementation
// ---------------------------------------------------------------------------
class UpdateService::Impl {
  public:
    RecordingCoordinator* coordinator = nullptr;
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    exosnap::update::InstallMode install_mode{};
    exosnap::update::UpdateState state{};
    // QCR-202. Single-flight admission and completion attribution are the same
    // fact, so they are the same field under the same mutex: a check is in
    // flight exactly while `active_operation` is non-zero, and only the
    // operation that still matches it may publish. `checking` used to be an
    // atomic the worker cleared *before* it wrote its results, which let a
    // second request start while the first was still publishing — two workers
    // writing `state` at once, and a UI that said "not checking" the whole time.
    std::uint64_t next_operation_id = 1;
    std::uint64_t active_operation = 0;
    // ADR 0055. Atomic because SetVerifyReinstallMode/IsVerifyReinstallMode run
    // outside the mutex; its value is snapshotted into the operation context at
    // check start so a toggle cannot change what a running check means.
    std::atomic<bool> verify_reinstall{false};
    mutable QMutex mutex;

    // WHATS-NEW: gap notes from the most recent completed check (mutex-guarded).
    std::vector<exosnap::update::ReleaseNote> gap_notes;

    // WHATS-NEW: the full channel-history notes from the most recent completed check
    // (mutex-guarded, mirrors gap_notes).
    std::vector<exosnap::update::ReleaseNote> all_channel_notes;

#if defined(_WIN32)
    HANDLE updater_process = nullptr;
    QWinEventNotifier* updater_exit_notifier = nullptr;
    qint64 updater_pid = 0;
#endif

    // Build the recording guard from the RecordingCoordinator's public API.
    // Reads `coordinator` under `mutex` since SetRecordingCoordinator() (called from
    // MainWindow on the UI thread once RecordPage finishes its deferred init) can
    // race with RequestUpdateCheck()'s background worker thread capturing the guard.
    exosnap::update::RecordingGuardFn MakeGuard() const {
        RecordingCoordinator* coord;
        {
            QMutexLocker lk(&mutex);
            coord = coordinator;
        }
        return [coord]() -> exosnap::update::UpdateBlockReason {
            if (!coord)
                return exosnap::update::UpdateBlockReason::NotBlocked;
            auto s = coord->State();
            if (s == UiRecordingState::Saving || s == UiRecordingState::Stopping)
                return exosnap::update::UpdateBlockReason::Finalizing;
            if (s == UiRecordingState::Recording || s == UiRecordingState::Paused ||
                s == UiRecordingState::ArmedFromRecovery || s == UiRecordingState::Preparing ||
                s == UiRecordingState::Countdown)
                return exosnap::update::UpdateBlockReason::ActiveRecording;
            return exosnap::update::UpdateBlockReason::NotBlocked;
        };
    }

    // Declared as the LAST data member so it is destroyed FIRST: its destructor
    // waits for the check worker, which writes `state`/`gap_notes` above under
    // `mutex` once CheckForUpdate returns. The previous worker was a detached
    // QThread nobody joined, so ~UpdateService's `delete impl_` could free that
    // mutex and that state while the worker was still about to lock and write
    // them. Never add a data member below this one.
    //
    // The wait has no deadline, because the engine offers none: CheckForUpdate
    // does a WinHTTP GET that installs no WinHttpSetTimeouts call and exposes
    // no cancel handle, so it ends only on WinHTTP's own connect/send/receive
    // defaults. Giving it a deadline is an update-engine policy decision and is
    // deliberately not invented here.
    QThreadPool check_pool;
};

// ---------------------------------------------------------------------------
// UpdateService
// ---------------------------------------------------------------------------
UpdateService::UpdateService(RecordingCoordinator* coordinator, QObject* parent) : QObject(parent), impl_(new Impl) {
    impl_->coordinator = coordinator;
    impl_->install_mode = exosnap::update::DetectInstallMode();
    impl_->state.channel = impl_->channel;
    impl_->state.install_mode = impl_->install_mode;
}

UpdateService::~UpdateService() {
#if defined(_WIN32)
    if (impl_->updater_exit_notifier != nullptr)
        impl_->updater_exit_notifier->setEnabled(false);
    if (impl_->updater_process != nullptr)
        ::CloseHandle(impl_->updater_process);
#endif
    // This is also the join: Impl's last member is a QThreadPool whose
    // destructor waits for the check worker, and it runs before the `mutex` and
    // `state` that worker writes are destroyed. Nothing the worker can touch
    // may be torn down above this line.
    delete impl_;
}

void UpdateService::SetRecordingCoordinator(RecordingCoordinator* coordinator) {
    QMutexLocker lk(&impl_->mutex);
    impl_->coordinator = coordinator;
}

exosnap::update::UpdateChannel UpdateService::Channel() const {
    // Under the mutex like every other read of it: SetChannel can run on the GUI
    // thread while a check worker is reading the same field.
    QMutexLocker lk(&impl_->mutex);
    return impl_->channel;
}

void UpdateService::SetChannel(exosnap::update::UpdateChannel ch) {
    QMutexLocker lk(&impl_->mutex);
    impl_->channel = ch;
    // "Update available — 1.2.0" was an answer about the previous feed; keeping
    // it under the new channel's label is the same misattribution the
    // completion gate prevents, only from the other direction.
    impl_->state = ApplyUpdateChannelSelection(impl_->state, ch);
}

void UpdateService::SetVerifyReinstallMode(bool on) {
    impl_->verify_reinstall.store(on);
    if (on)
        diagnostics::AppLog::info(
            QStringLiteral("update"),
            QStringLiteral("Verification reinstall mode active — the running version %1 may be reinstalled "
                           "through the normal update path (this run only; nothing was persisted)")
                .arg(QString::fromLatin1(exosnap::build::kVersion)));
}

bool UpdateService::IsVerifyReinstallMode() const {
    return impl_->verify_reinstall.load();
}

exosnap::update::UpdateBlockReason UpdateService::CurrentBlockReason() const {
    auto guard = impl_->MakeGuard();
    return guard ? guard() : exosnap::update::UpdateBlockReason::NotBlocked;
}

exosnap::update::UpdateState UpdateService::CurrentState() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->state;
}

std::vector<exosnap::update::ReleaseNote> UpdateService::LastGapNotes() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->gap_notes;
}

std::vector<exosnap::update::ReleaseNote> UpdateService::LastAllChannelNotes() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->all_channel_notes;
}

void UpdateService::RequestUpdateCheck() {
    auto* impl = impl_;
    auto* self = this;

    // QCR-202. Admission and the operation context are one critical section: the
    // channel and the verification mode are read here, on the requesting thread,
    // and travel with the operation. Reading them inside the worker meant the
    // check could query one feed and be interpreted as another.
    UpdateCheckOperation operation;
    {
        QMutexLocker lk(&impl_->mutex);
        if (impl_->active_operation != 0)
            return; // already in progress
        operation.id = impl_->next_operation_id++;
        operation.channel = impl_->channel;
        operation.verify_reinstall = impl_->verify_reinstall.load();
        impl_->active_operation = operation.id;
        impl_->state.checking = true;
        impl_->state.channel = operation.channel;
    }

    impl_->check_pool.start([impl, self, operation]() {
        namespace upd = exosnap::update;

        upd::CheckParams params;
        params.current_version = upd::ParseSemVer(exosnap::build::kVersion).value_or(upd::SemVer{0, 0, 0});
        // kVersion is treated as an opaque full version string ("0.9.0-rc4",
        // "0.9.0-dev"): the verification gate compares it byte-for-byte, so a
        // build whose identity does not match any release tag simply never
        // qualifies — which is the safe direction.
        params.current_version_raw = exosnap::build::kVersion;
        params.allow_same_version_reinstall = operation.verify_reinstall;
        params.channel = operation.channel;
        params.recording_guard = impl->MakeGuard();

        const auto result = upd::CheckForUpdate(params);

        UpdateCheckCompletion completion;
        upd::UpdateChannel channel_now = operation.channel;
        {
            QMutexLocker lk(&impl->mutex);
            completion =
                ResolveUpdateCheckCompletion(impl->state, operation, impl->active_operation, impl->channel, result);
            if (operation.id == impl->active_operation)
                impl->active_operation = 0;
            if (completion.publish)
                impl->state = completion.state;
            if (completion.adopt_notes) {
                impl->gap_notes = result.gap_notes;
                impl->all_channel_notes = result.all_channel_notes;
            } else if (completion.verdict == UpdateCompletionVerdict::ChannelChanged) {
                // Notes for a feed the user has left must not survive into the
                // pending "What's new" payload LaunchUpdater writes.
                impl->gap_notes.clear();
                impl->all_channel_notes.clear();
            }
            channel_now = impl->channel;
        }

        switch (completion.verdict) {
        case UpdateCompletionVerdict::SupersededByNewerCheck:
            // The newer operation owns the state and will publish its own; this
            // one says nothing at all.
            diagnostics::AppLog::info(
                QStringLiteral("update"),
                QStringLiteral("Discarded the result of update check %1: a newer check has since started")
                    .arg(operation.id));
            return;
        case UpdateCompletionVerdict::ChannelChanged:
            diagnostics::AppLog::info(
                QStringLiteral("update"),
                QStringLiteral("Discarded the result of update check %1: it ran on the %2 channel and the "
                               "selection has since moved to %3")
                    .arg(operation.id)
                    .arg(UpdateChannelToString(operation.channel), UpdateChannelToString(channel_now)));
            break;
        case UpdateCompletionVerdict::Adopt:
            break;
        }

        const bool deliver_result = completion.verdict == UpdateCompletionVerdict::Adopt;
        const auto operation_channel = operation.channel;
        QMetaObject::invokeMethod(
            self,
            [self, result, deliver_result, operation_channel]() {
                // Checked again here, not only in the worker: this call was
                // queued, so the channel can have changed between the worker
                // publishing and the GUI thread delivering. Without this the
                // card could still be filled in from the other feed's answer.
                if (deliver_result && self->Channel() == operation_channel)
                    emit self->updateCheckComplete(result);
                emit self->updateStateChanged(self->CurrentState());
            },
            Qt::QueuedConnection);
    });
}

void UpdateService::LaunchUpdater() {
    namespace upd = exosnap::update;

    // Recording guard: never start a swap while a capture or MP4 remux is in
    // flight (same policy as the check path). The UI keeps the button disabled,
    // but guard defensively here too.
    if (CurrentBlockReason() != upd::UpdateBlockReason::NotBlocked) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't update while recording or finalizing."));
        return;
    }

    const QString app_dir = QCoreApplication::applicationDirPath();

    // Stage into %LOCALAPPDATA%\<org>\ExoSnap\updater\ (AppLocalDataLocation +
    // "/updater"). Running the updater from a separate directory lets it replace
    // the app's own files while the app is still shutting down.
    const QString local_data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (local_data.isEmpty()) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't locate a staging directory for the updater."));
        return;
    }
    const QString staging_dir = QDir(local_data).filePath(QStringLiteral("updater"));

    // Wipe any prior staging and recreate a clean tree.
    QDir staging(staging_dir);
    if (staging.exists())
        staging.removeRecursively();
    if (!QDir().mkpath(staging_dir)) {
        emit updateError(upd::VerifyResult::PackageNotFound,
                         QStringLiteral("Can't create the updater staging directory."));
        return;
    }

    // QCR-203. A staging step that fails must not leave a half-built updater
    // behind in %LOCALAPPDATA% for the user to find and double-click. Every
    // required-step failure exits through here.
    const auto fail_staging = [this, staging_dir](const QString& detail) {
        QDir(staging_dir).removeRecursively();
        emit updateError(exosnap::update::VerifyResult::PackageNotFound, detail);
    };

    // Copy the mandatory runtime subset; a missing entry is a hard failure.
    for (const QString& rel : UpdaterStagingFileList()) {
        const QString src = QDir(app_dir).filePath(rel);
        const QString dst = QDir(staging_dir).filePath(rel);
        if (!QFileInfo::exists(src)) {
            fail_staging(QStringLiteral("Updater runtime file missing: %1").arg(rel));
            return;
        }
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(src, dst)) {
            fail_staging(QStringLiteral("Failed to stage updater file: %1").arg(rel));
            return;
        }
    }

    // Best-effort, and genuinely optional: without the styles plugin the updater
    // falls back to Qt's built-in style. It looks different; it still runs and
    // still swaps the installation correctly.
    const QDir styles_src(QDir(app_dir).filePath(QStringLiteral("plugins/styles")));
    if (styles_src.exists()) {
        const QString styles_dst_dir = QDir(staging_dir).filePath(QStringLiteral("plugins/styles"));
        QDir().mkpath(styles_dst_dir);
        for (const QFileInfo& fi : styles_src.entryInfoList(QStringList{QStringLiteral("*.dll")}, QDir::Files)) {
            if (!QFile::copy(fi.absoluteFilePath(), QDir(styles_dst_dir).filePath(fi.fileName()))) {
                diagnostics::AppLog::warning(
                    QStringLiteral("update"),
                    QStringLiteral("Optional updater style plugin %1 was not staged; the updater will use Qt's "
                                   "built-in style.")
                        .arg(fi.fileName()));
            }
        }
    }

    // QCR-203, required: the platform plugin is staged to plugins/platforms/,
    // and Qt's default library paths cover the application directory and the
    // *build-time* Qt plugins path — neither of which exists on a user machine.
    // Without this file the staged updater cannot load qwindows.dll and dies at
    // startup with "could not load the Qt platform plugin". ADR 0037 §E treats
    // it as part of the staging contract for exactly that reason: the packaging
    // gate's updater smoke reproduces this write verbatim. So its failure is a
    // staging failure, not something to shrug at — the previous code checked
    // only whether the file opened and ignored the write and the close.
    {
        const QByteArray qt_conf_bytes("[Paths]\nPlugins = plugins\n");
        QFile qt_conf(QDir(staging_dir).filePath(QStringLiteral("qt.conf")));
        if (!qt_conf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail_staging(QStringLiteral("Failed to write the updater's qt.conf: %1").arg(qt_conf.errorString()));
            return;
        }
        const qint64 written = qt_conf.write(qt_conf_bytes);
        // flush() before close(): QFile::close() reports nothing, and a buffered
        // write that fails on flush would otherwise pass unnoticed — the same
        // failure mode QCR-108 found in the output-folder probe.
        const bool flushed = qt_conf.flush();
        qt_conf.close();
        if (written != qt_conf_bytes.size() || !flushed || qt_conf.error() != QFileDevice::NoError) {
            fail_staging(QStringLiteral("Failed to write the updater's qt.conf: %1").arg(qt_conf.errorString()));
            return;
        }
    }

    const QString staged_exe = QDir(staging_dir).filePath(QStringLiteral("exosnap-updater.exe"));

    // WHATS-NEW: persist the gap notes as a pending payload BEFORE launching, so
    // the one-time "What's new" overlay can show on the first launch of the new
    // build. Only meaningful when the last check produced notes for the target
    // version. If there is nothing to show, clear any stale payload instead.
    {
        std::vector<upd::ReleaseNote> notes;
        std::optional<upd::SemVer> target;
        {
            QMutexLocker lk(&impl_->mutex);
            notes = impl_->gap_notes;
            target = impl_->state.available_version;
        }
        const QString payload_path = WhatsNewPayloadPath();
        if (target && !notes.empty()) {
            WhatsNewPendingPayload payload;
            payload.target_version = QString::fromStdString(target->ToString());
            for (const auto& n : notes) {
                WhatsNewNote note;
                note.version = QString::fromStdString(n.version.ToString());
                note.body = QString::fromStdString(n.body_markdown);
                note.html_url = QString::fromStdString(n.html_url);
                payload.notes.push_back(note);
            }
            // QCR-203, optional: this payload is presentation only. Losing it
            // costs the one-time "What's new" overlay on the next launch and
            // nothing else — the update itself, its verification and the swap
            // are entirely unaffected — so a failure is reported, not fatal.
            // Aborting the update over release notes would be the worse bug.
            if (!WriteWhatsNewPayload(payload_path, payload)) {
                diagnostics::AppLog::warning(
                    QStringLiteral("update"),
                    QStringLiteral("Could not write the pending \"What's new\" payload to %1; the update "
                                   "continues, but the post-update notes overlay will not appear.")
                        .arg(payload_path));
                // A stale payload from an earlier update would now claim to
                // describe this one, so it goes rather than being left behind.
                DeleteWhatsNewPayload(payload_path);
            }
        } else {
            DeleteWhatsNewPayload(payload_path);
        }
    }

#if defined(_WIN32)
    const quint32 pid = static_cast<quint32>(::GetCurrentProcessId());
    const bool verify_reinstall = impl_->verify_reinstall.load();
    if (verify_reinstall)
        diagnostics::AppLog::info(QStringLiteral("update"),
                                  QStringLiteral("Launching the updater in verification reinstall mode — it will "
                                                 "refuse any version but %1")
                                      .arg(QString::fromLatin1(exosnap::build::kVersion)));
    // Snapshotted, not read in place: BuildUpdaterArgs reads state.channel and
    // state.install_mode, and a check worker can be writing that same struct.
    const QStringList flags =
        BuildUpdaterArgs(CurrentState(), app_dir, pid, QString::fromLatin1(exosnap::build::kVersion), verify_reinstall);

    // Launch detached: the app does not wait — the updater sends WM_CLOSE when it is
    // ready to swap. QProcess applies the correct Windows argument-quoting rules so
    // paths/args with spaces or quotes are passed through safely.
    qint64 updater_pid = 0;
    const bool ok = QProcess::startDetached(QDir::toNativeSeparators(staged_exe), flags,
                                            QDir::toNativeSeparators(staging_dir), &updater_pid);
    if (!ok) {
        emit updateError(upd::VerifyResult::PackageNotFound, QStringLiteral("Failed to launch the updater."));
        return;
    }

    // Keep an event-driven watch on the detached process while the old app is
    // alive. If the user closes a failed/cancelled updater before the marked
    // close handoff, MainWindow can immediately re-arm the card. This is process
    // lifecycle ownership, not UI polling.
    impl_->updater_pid = updater_pid;
    impl_->updater_process =
        ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(updater_pid));
    if (impl_->updater_process != nullptr) {
        impl_->updater_exit_notifier = new QWinEventNotifier(impl_->updater_process, this);
        connect(impl_->updater_exit_notifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
            DWORD exit_code = 0;
            if (!::GetExitCodeProcess(impl_->updater_process, &exit_code))
                exit_code = static_cast<DWORD>(-1);

            impl_->updater_exit_notifier->setEnabled(false);
            impl_->updater_exit_notifier->deleteLater();
            impl_->updater_exit_notifier = nullptr;
            ::CloseHandle(impl_->updater_process);
            impl_->updater_process = nullptr;

            const qint64 process_id = impl_->updater_pid;
            impl_->updater_pid = 0;
            emit updaterExited(process_id, static_cast<quint32>(exit_code));
        });
    } else {
        diagnostics::AppLog::warning(
            QStringLiteral("update"),
            QStringLiteral("Updater launched, but its process lifetime could not be observed (pid %1).")
                .arg(updater_pid));
    }
    emit updaterLaunched();
#else
    emit updateError(upd::VerifyResult::PackageNotFound, QStringLiteral("The updater is only available on Windows."));
#endif
}

void UpdateService::HandoffToInstaller(const QString& installer_path) {
    namespace upd = exosnap::update;
    bool ok = upd::HandoffToInstaller(installer_path.toStdString());
    if (!ok) {
        emit updateError(upd::VerifyResult::PackageNotFound, "Failed to launch installer");
    }
}

} // namespace exosnap
