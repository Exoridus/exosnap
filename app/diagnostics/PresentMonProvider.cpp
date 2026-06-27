#include "PresentMonProvider.h"

namespace exosnap::diagnostics {

PresentMonProvider::PresentMonProvider(const IElevationProvider& elevation, bool opt_in)
    : elevation_(elevation), opt_in_(opt_in) {
}

bool PresentMonProvider::IsAvailable() const {
    // Gate: opt-in AND elevation. The Vendor-Slice adds "&& etw_session_open_".
    return opt_in_ && elevation_.IsElevated();
}

PresentSample PresentMonProvider::Sample() const {
    // TODO Vendor-Slice: in-process PresentMon ETW consumer; until then Unavailable.
    // We deliberately return the default sample (available == false) even when the
    // gate is open, because no ETW datum exists yet — never fabricate a present.
    return PresentSample{};
}

void PresentMonProvider::SetOptIn(bool opt_in) {
    opt_in_ = opt_in;
}

} // namespace exosnap::diagnostics
