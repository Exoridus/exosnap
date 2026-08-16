#include "BlockingSurfaceArbiter.h"

#include "CrashReportAdapter.h"
#include "RecordingErrorAdapter.h"
#include "RecoveryAdapter.h"

#include <algorithm>

namespace exosnap::quick {

BlockingSurfaceArbiter::BlockingSurfaceArbiter(QObject* parent) : QObject(parent) {
}

void BlockingSurfaceArbiter::setSurfaces(RecoveryAdapter* recovery, CrashReportAdapter* crash,
                                         RecordingErrorAdapter* recording_error) {
    if (recovery_ != nullptr)
        disconnect(recovery_, nullptr, this, nullptr);
    if (crash_ != nullptr)
        disconnect(crash_, nullptr, this, nullptr);
    if (recording_error_ != nullptr)
        disconnect(recording_error_, nullptr, this, nullptr);

    recovery_ = recovery;
    crash_ = crash;
    recording_error_ = recording_error;

    if (recovery_ != nullptr)
        connect(recovery_, &RecoveryAdapter::surfaceOpenChanged, this, &BlockingSurfaceArbiter::onSurfaceStateChanged);
    // CrashReportAdapter and RecordingErrorAdapter each have one aggregate notify
    // signal; `active` is what this reads out of them.
    if (crash_ != nullptr)
        connect(crash_, &CrashReportAdapter::changed, this, &BlockingSurfaceArbiter::onSurfaceStateChanged);
    if (recording_error_ != nullptr) {
        connect(recording_error_, &RecordingErrorAdapter::changed, this,
                &BlockingSurfaceArbiter::onSurfaceStateChanged);
    }
}

bool BlockingSurfaceArbiter::available(Surface surface) const {
    switch (surface) {
    case Surface::Recovery:
        // Raised by calling the adapter, so without one there is nothing to raise.
        return recovery_ != nullptr;
    case Surface::Crash:
    case Surface::RecordingError:
        // Raised as a signal the composition root answers. It owns what these
        // surfaces show, so a missing adapter pointer does not make the request
        // meaningless — it only means this object cannot observe the surface,
        // and an unobservable surface reports "not up" and blocks nothing.
        return true;
    }
    return false;
}

bool BlockingSurfaceArbiter::isUp(Surface surface) const {
    switch (surface) {
    case Surface::Recovery:
        return recovery_ != nullptr && recovery_->surfaceOpen();
    case Surface::Crash:
        return crash_ != nullptr && crash_->active();
    case Surface::RecordingError:
        return recording_error_ != nullptr && recording_error_->active();
    }
    return false;
}

bool BlockingSurfaceArbiter::anyUp() const {
    return isUp(Surface::Recovery) || isUp(Surface::Crash) || isUp(Surface::RecordingError);
}

bool BlockingSurfaceArbiter::queued(Surface surface) const noexcept {
    return std::find(queue_.begin(), queue_.end(), surface) != queue_.end();
}

void BlockingSurfaceArbiter::request(Surface surface) {
    if (!available(surface))
        return;

    // Something else already owns the screen: queue behind it. Note the test is
    // "any OTHER surface", not "any surface" — a repeated request for the surface
    // that is already up is a refresh (a second failed recording replaces the
    // first report), never a request to queue behind itself.
    const bool blocked = (surface != Surface::Recovery && isUp(Surface::Recovery)) ||
                         (surface != Surface::Crash && isUp(Surface::Crash)) ||
                         (surface != Surface::RecordingError && isUp(Surface::RecordingError));
    if (blocked) {
        if (!queued(surface))
            queue_.push_back(surface);
        return;
    }

    // Raising it now settles any earlier queue entry for the same surface: the
    // request is being answered, not deferred a second time.
    std::erase(queue_, surface);
    raise(surface);
}

void BlockingSurfaceArbiter::raise(Surface surface) {
    switch (surface) {
    case Surface::Recovery:
        if (recovery_ != nullptr)
            recovery_->openSurface();
        return;
    case Surface::Crash:
        emit crashSurfaceRequested();
        return;
    case Surface::RecordingError:
        emit recordingErrorSurfaceRequested();
        return;
    }
}

void BlockingSurfaceArbiter::requestRecovery() {
    request(Surface::Recovery);
}

void BlockingSurfaceArbiter::requestCrash() {
    request(Surface::Crash);
}

void BlockingSurfaceArbiter::requestRecordingError() {
    request(Surface::RecordingError);
}

bool BlockingSurfaceArbiter::recoveryQueued() const noexcept {
    return queued(Surface::Recovery);
}

bool BlockingSurfaceArbiter::crashQueued() const noexcept {
    return queued(Surface::Crash);
}

bool BlockingSurfaceArbiter::recordingErrorQueued() const noexcept {
    return queued(Surface::RecordingError);
}

bool BlockingSurfaceArbiter::anySurfaceUp() const {
    return anyUp();
}

void BlockingSurfaceArbiter::onSurfaceStateChanged() {
    // Raising a surface emits the signal that got us here. Without this guard a
    // queued request would be evaluated again from inside its own dispatch,
    // against a state that is only half written.
    if (dispatching_)
        return;
    if (queue_.empty() || anyUp())
        return;

    dispatching_ = true;
    const Surface next = queue_.front();
    queue_.erase(queue_.begin());
    raise(next);
    dispatching_ = false;
}

} // namespace exosnap::quick
