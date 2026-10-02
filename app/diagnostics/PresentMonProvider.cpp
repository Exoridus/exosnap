#include "diagnostics/PresentMonProvider.h"

#include <chrono>
#include <utility>

namespace exosnap::diagnostics {

PresentMonProvider::PresentMonProvider(bool opt_in) : opt_in_(opt_in) {
    if (GateOpen()) {
        [[maybe_unused]] bool started = session_.Start(); // graceful: false when the trace can't open
    }
}

PresentMonProvider::PresentMonProvider(bool opt_in,
                                       std::function<std::shared_ptr<IPresentTraceBackend>()> backend_factory)
    : opt_in_(opt_in), session_(std::move(backend_factory)) {
    if (GateOpen()) {
        [[maybe_unused]] bool started = session_.Start();
    }
}

bool PresentMonProvider::GateOpen() const {
    return opt_in_;
}

bool PresentMonProvider::IsAvailable() const {
    return GateOpen() && session_.IsOpen();
}

PresentProviderState PresentMonProvider::state() const {
    if (!GateOpen())
        return PresentProviderState::NotRequested;
    if (session_.IsOpen()) {
        const PresentSample sample = session_.Latest();
        if (sample.available && sample.metadata.Fresh(std::chrono::steady_clock::now()))
            return PresentProviderState::Measuring;
        // An open trace nobody has produced a present for yet. Deliberately not
        // "Measuring" and deliberately not a zero measurement.
        return PresentProviderState::OpenNoData;
    }
    switch (session_.OpenResult()) {
    case PresentTraceOpenResult::Opened:
        // It opened; the consumer has since ended. That is a stop, not a
        // measurement.
        return PresentProviderState::Stopped;
    case PresentTraceOpenResult::NotBuilt:
        return PresentProviderState::NotBuilt;
    case PresentTraceOpenResult::AccessDenied:
        return PresentProviderState::AccessDenied;
    case PresentTraceOpenResult::SessionConflict:
        return PresentProviderState::SessionConflict;
    case PresentTraceOpenResult::NotSupported:
        return PresentProviderState::NotSupported;
    case PresentTraceOpenResult::Failed:
        break;
    }
    return PresentProviderState::Failed;
}

PresentTraceOpenResult PresentMonProvider::openResult() const {
    return session_.OpenResult();
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
