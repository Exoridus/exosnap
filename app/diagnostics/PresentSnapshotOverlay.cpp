#include "diagnostics/PresentSnapshotOverlay.h"

namespace exosnap::diagnostics {

recorder_core::PresentMode ToSnapshotPresentMode(PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::Composed:
        return recorder_core::PresentMode::Composed;
    case PresentMode::IndependentFlip:
        return recorder_core::PresentMode::IndependentFlip;
    case PresentMode::ExclusiveFullscreen:
        return recorder_core::PresentMode::ExclusiveFullscreen;
    case PresentMode::Unknown:
        break;
    }
    return recorder_core::PresentMode::Unknown;
}

void ApplyPresentSample(recorder_core::CaptureDiagnostics& capture, const std::optional<PresentSample>& sample) {
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
    capture.present_mode_availability = recorder_core::MetricAvailability::Available;
}

} // namespace exosnap::diagnostics
