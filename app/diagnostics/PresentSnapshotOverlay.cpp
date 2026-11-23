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

void ApplyPresentSample(exosnap::engine::CaptureDiagnostics& capture, const std::optional<PresentSample>& sample,
                        unsigned long target_pid, std::chrono::steady_clock::time_point now) {
    if (target_pid == 0 || !sample || !sample->FreshForTarget(target_pid, now) || sample->mode == PresentMode::Unknown)
        return;

    capture.source_present_mode = ToSnapshotPresentMode(sample->mode);
    capture.source_tearing = sample->tearing;
    capture.present_mode_availability = exosnap::engine::MetricAvailability::Available;
}

} // namespace exosnap::diagnostics
