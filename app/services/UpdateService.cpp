// UpdateService.cpp -- Qt bridge implementation for the update engine.

#include "UpdateService.h"

#include <update/install_mode_detector.h>
#include <update/manifest_io.h>
#include <update/package_verifier.h>
#include <update/update_checker.h>
#include <update/update_types.h>

#include "RecordingCoordinator.h"

#include "../viewmodels/RecordViewModel.h" // for UiRecordingState

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QThread>
#include <atomic>

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
        params.current_version = upd::ParseSemVer("0.3.0").value_or(upd::SemVer{0, 0, 0});
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

void UpdateService::RequestDownloadAndVerify() {
    if (impl_->install_mode != exosnap::update::InstallMode::Installed) {
        emit updateError(exosnap::update::VerifyResult::PackageNotFound, "Download not available in portable mode");
        return;
    }
    // TODO(Update-C): implement download via WinHTTP + temp file +
    //   VerifyPackage() + emit packageReadyForInstall or updateError.
    //   This stub satisfies the UI seam contract; implementation follows in Update-C slice.
}

void UpdateService::HandoffToInstaller(const QString& installer_path) {
    namespace upd = exosnap::update;
    bool ok = upd::HandoffToInstaller(installer_path.toStdString());
    if (!ok) {
        emit updateError(upd::VerifyResult::PackageNotFound, "Failed to launch installer");
    }
}

} // namespace exosnap
