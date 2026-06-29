#include "diagnostics/PresentMonProvider.h"

namespace exosnap::diagnostics {

PresentMonProvider::PresentMonProvider(const IElevationProvider& elevation, bool opt_in)
    : elevation_(elevation), opt_in_(opt_in) {
    if (GateOpen()) {
        [[maybe_unused]] bool started = session_.Start(); // graceful: false when ETW can't open
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
