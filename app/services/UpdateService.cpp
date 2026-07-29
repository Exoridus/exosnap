// UpdateService.cpp -- Qt bridge implementation for the update engine.

#include "UpdateService.h"

#include <update/install_mode_detector.h>
#include <update/manifest_io.h>
#include <update/package_verifier.h>
#include <update/update_checker.h>
#include <update/update_types.h>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion (generated from PROJECT_VERSION)
#include "RecordingCoordinator.h"
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
#include <QThread>
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
    std::atomic<bool> checking{false};
    // ADR 0055. Atomic because the check worker thread reads it.
    std::atomic<bool> verify_reinstall{false};
    mutable QMutex mutex;

    // WHATS-NEW: gap notes from the most recent completed check (mutex-guarded).
    std::vector<exosnap::update::ReleaseNote> gap_notes;

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
    delete impl_;
}

void UpdateService::SetRecordingCoordinator(RecordingCoordinator* coordinator) {
    QMutexLocker lk(&impl_->mutex);
    impl_->coordinator = coordinator;
}

exosnap::update::UpdateChannel UpdateService::Channel() const {
    return impl_->channel;
}

void UpdateService::SetChannel(exosnap::update::UpdateChannel ch) {
    QMutexLocker lk(&impl_->mutex);
    impl_->channel = ch;
    impl_->state.channel = ch;
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

void UpdateService::RequestUpdateCheck() {
    if (impl_->checking.exchange(true))
        return; // already in progress

    auto* impl = impl_;
    auto* self = this;

    QThread* worker = QThread::create([impl, self]() {
        namespace upd = exosnap::update;

        upd::CheckParams params;
        params.current_version = upd::ParseSemVer(exosnap::build::kVersion).value_or(upd::SemVer{0, 0, 0});
        // kVersion is treated as an opaque full version string ("0.9.0-rc4",
        // "0.9.0-dev"): the verification gate compares it byte-for-byte, so a
        // build whose identity does not match any release tag simply never
        // qualifies — which is the safe direction.
        params.current_version_raw = exosnap::build::kVersion;
        params.allow_same_version_reinstall = impl->verify_reinstall.load();
        params.channel = impl->channel;
        params.recording_guard = impl->MakeGuard();

        auto result = upd::CheckForUpdate(params);
        impl->checking = false;

        {
            QMutexLocker lk(&impl->mutex);
            impl->state.checking = false;
            impl->state.update_available = result.update_available;
            impl->state.available_version = result.available_version;
            if (result.error_message)
                impl->state.last_error = *result.error_message;
            impl->gap_notes = result.gap_notes;
        }

        QMetaObject::invokeMethod(
            self,
            [self, result]() {
                emit self->updateCheckComplete(result);
                emit self->updateStateChanged(self->impl_->state);
            },
            Qt::QueuedConnection);
    });

    {
        QMutexLocker lk(&impl_->mutex);
        impl_->state.checking = true;
    }
    worker->start();
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
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

    // Copy the mandatory runtime subset; a missing entry is a hard failure.
    for (const QString& rel : UpdaterStagingFileList()) {
        const QString src = QDir(app_dir).filePath(rel);
        const QString dst = QDir(staging_dir).filePath(rel);
        if (!QFileInfo::exists(src)) {
            emit updateError(upd::VerifyResult::PackageNotFound,
                             QStringLiteral("Updater runtime file missing: %1").arg(rel));
            return;
        }
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(src, dst)) {
            emit updateError(upd::VerifyResult::PackageNotFound,
                             QStringLiteral("Failed to stage updater file: %1").arg(rel));
            return;
        }
    }

    // Best-effort: copy the styles plugin(s) if present (optional).
    const QDir styles_src(QDir(app_dir).filePath(QStringLiteral("plugins/styles")));
    if (styles_src.exists()) {
        const QString styles_dst_dir = QDir(staging_dir).filePath(QStringLiteral("plugins/styles"));
        QDir().mkpath(styles_dst_dir);
        for (const QFileInfo& fi : styles_src.entryInfoList(QStringList{QStringLiteral("*.dll")}, QDir::Files))
            QFile::copy(fi.absoluteFilePath(), QDir(styles_dst_dir).filePath(fi.fileName()));
    }

    // qt.conf so the staged updater finds its plugins under ./plugins.
    {
        QFile qt_conf(QDir(staging_dir).filePath(QStringLiteral("qt.conf")));
        if (qt_conf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qt_conf.write("[Paths]\nPlugins = plugins\n");
            qt_conf.close();
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
            WriteWhatsNewPayload(payload_path, payload);
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
    const QStringList flags =
        BuildUpdaterArgs(impl_->state, app_dir, pid, QString::fromLatin1(exosnap::build::kVersion), verify_reinstall);

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
