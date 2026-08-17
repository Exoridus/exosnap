#include "diagnostics/PresentMonProvider.h"

#include <utility>

namespace exosnap::diagnostics {

PresentMonProvider::PresentMonProvider(const IElevationProvider& elevation, bool opt_in)
    : elevation_(elevation), opt_in_(opt_in) {
    if (GateOpen()) {
        [[maybe_unused]] bool started = session_.Start(); // graceful: false when the trace can't open
    }
}

PresentMonProvider::PresentMonProvider(const IElevationProvider& elevation, bool opt_in,
                                       std::function<std::shared_ptr<IPresentTraceBackend>()> backend_factory)
    : elevation_(elevation), opt_in_(opt_in), session_(std::move(backend_factory)) {
    if (GateOpen()) {
        [[maybe_unused]] bool started = session_.Start();
    }
}

bool PresentMonProvider::GateOpen() const {
    return opt_in_ && elevation_.IsElevated();
}

bool PresentMonProvider::IsAvailable() const {
    return GateOpen() && session_.IsOpen();
}

PresentSample PresentMonProvider::Sample() const {
    if (!IsAvailable()) {
        return PresentSample{}; // Unavailable — never fabricate
    }
    return session_.Latest();
}

void PresentMonProvider::SetOptIn(bool opt_in) {
    opt_in_ = opt_in;
    if (GateOpen()) {
        [[maybe_unused]] bool started = session_.Start();
    } else {
        session_.Stop();
    }
}

} // namespace exosnap::diagnostics
