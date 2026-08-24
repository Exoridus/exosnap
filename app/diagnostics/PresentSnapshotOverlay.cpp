#include "diagnostics/PresentSnapshotOverlay.h"

namespace exosnap::diagnostics {

exosnap::engine::PresentMode ToSnapshotPresentMode(PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::Composed:
        return exosnap::engine::PresentMode::Composed;
    case PresentMode::IndependentFlip:
        return exosnap::engine::PresentMode::IndependentFlip;
    case PresentMode::ExclusiveFullscreen:
        return exosnap::engine::PresentMode::ExclusiveFullscreen;
    case PresentMode::Unknown:
        break;
    }
    return exosnap::engine::PresentMode::Unknown;
}

void ApplyPresentSample(exosnap::engine::CaptureDiagnostics& capture, const std::optional<PresentSample>& sample) {
    if (!sample.has_value() || !sample->available)
        return;
    // A session that is open but has not yet decoded a present reports available ==
    // true with mode == Unknown. Publishing that as an available measurement would
    // put "Unknown" in front of the user as a present-mode verdict; it is the
    // absence of one.
    if (sample->mode == PresentMode::Unknown)
        return;

    capture.source_present_mode = ToSnapshotPresentMode(sample->mode);
    capture.source_tearing = sample->tearing;
    capture.present_mode_availability = exosnap::engine::MetricAvailability::Available;
}

} // namespace exosnap::diagnostics
