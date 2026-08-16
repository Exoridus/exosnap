#include "BlockingSurfaceArbiter.h"

#include "CrashReportAdapter.h"
#include "RecoveryAdapter.h"

namespace exosnap::quick {

BlockingSurfaceArbiter::BlockingSurfaceArbiter(QObject* parent) : QObject(parent) {
}

void BlockingSurfaceArbiter::setSurfaces(RecoveryAdapter* recovery, CrashReportAdapter* crash) {
    if (recovery_ != nullptr)
        disconnect(recovery_, nullptr, this, nullptr);
    if (crash_ != nullptr)
        disconnect(crash_, nullptr, this, nullptr);

    recovery_ = recovery;
    crash_ = crash;

    if (recovery_ != nullptr) {
        connect(recovery_, &RecoveryAdapter::surfaceOpenChanged, this, &BlockingSurfaceArbiter::onSurfaceStateChanged);
    }
    // CrashReportAdapter has one aggregate notify signal; `active` is what this
    // reads out of it.
    if (crash_ != nullptr)
        connect(crash_, &CrashReportAdapter::changed, this, &BlockingSurfaceArbiter::onSurfaceStateChanged);
}

bool BlockingSurfaceArbiter::recoveryUp() const {
    return recovery_ != nullptr && recovery_->surfaceOpen();
}

bool BlockingSurfaceArbiter::crashUp() const {
    return crash_ != nullptr && crash_->active();
}

void BlockingSurfaceArbiter::requestRecovery() {
    if (recovery_ == nullptr)
        return;
    if (crashUp()) {
        recovery_queued_ = true;
        return;
    }
    recovery_queued_ = false;
    recovery_->openSurface();
}

void BlockingSurfaceArbiter::requestCrash() {
    if (recoveryUp()) {
        crash_queued_ = true;
        return;
    }
    crash_queued_ = false;
    emit crashSurfaceRequested();
}

bool BlockingSurfaceArbiter::recoveryQueued() const noexcept {
    return recovery_queued_;
}

bool BlockingSurfaceArbiter::crashQueued() const noexcept {
    return crash_queued_;
}

void BlockingSurfaceArbiter::onSurfaceStateChanged() {
    // Raising a surface emits the signal that got us here. Without this guard a
    // queued request would be evaluated again from inside its own dispatch,
    // against a state that is only half written.
    if (dispatching_)
        return;
    dispatching_ = true;

    if (recovery_queued_ && !crashUp()) {
        recovery_queued_ = false;
        recovery_->openSurface();
    } else if (crash_queued_ && !recoveryUp()) {
        crash_queued_ = false;
        emit crashSurfaceRequested();
    }

    dispatching_ = false;
}

} // namespace exosnap::quick
