// UpdateService.cpp -- Qt bridge implementation for the update engine.

#include "UpdateService.h"

#include <update/install_mode_detector.h>
#include <update/manifest_io.h>
#include <update/package_verifier.h>
#include <update/update_checker.h>
#include <update/update_types.h>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion (generated from PROJECT_VERSION)
#include "RecordingCoordinator.h"

#include "../viewmodels/RecordViewModel.h" // for UiRecordingState

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThread>
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
    mutable QMutex mutex;

    // Build the recording guard from the RecordingCoordinator's public API.
    exosnap::update::RecordingGuardFn MakeGuard() const {
        RecordingCoordinator* coord = coordinator;
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
    delete impl_;
}

exosnap::update::UpdateChannel UpdateService::Channel() const {
    return impl_->channel;
}

void UpdateService::SetChannel(exosnap::update::UpdateChannel ch) {
    QMutexLocker lk(&impl_->mutex);
    impl_->channel = ch;
    impl_->state.channel = ch;
}

exosnap::update::UpdateBlockReason UpdateService::CurrentBlockReason() const {
    auto guard = impl_->MakeGuard();
    return guard ? guard() : exosnap::update::UpdateBlockReason::NotBlocked;
}

exosnap::update::UpdateState UpdateService::CurrentState() const {
    QMutexLocker lk(&impl_->mutex);
    return impl_->state;
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

#if defined(_WIN32)
    const quint32 pid = static_cast<quint32>(::GetCurrentProcessId());
    const QStringList flags =
        BuildUpdaterArgs(impl_->state, app_dir, pid, QString::fromLatin1(exosnap::build::kVersion));

    // Build a single quoted command line for CreateProcessW.
    QString command_line = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(staged_exe));
    for (const QString& arg : flags)
        command_line += QStringLiteral(" \"%1\"").arg(arg);

    std::wstring exe_w = QDir::toNativeSeparators(staged_exe).toStdWString();
    std::wstring cmd_w = command_line.toStdWString();
    std::wstring cwd_w = QDir::toNativeSeparators(staging_dir).toStdWString();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL ok =
        ::CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd_w.c_str(), &si, &pi);
    if (!ok) {
        emit updateError(upd::VerifyResult::PackageNotFound, QStringLiteral("Failed to launch the updater."));
        return;
    }
    // The app does not wait: the updater sends WM_CLOSE when it is ready to swap.
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
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
