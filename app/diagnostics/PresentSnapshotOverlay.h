#pragma once

#include "PresentProvider.h"

#include <optional>

#include <recorder_core/pipeline_diagnostics.h>

namespace exosnap::diagnostics {

// Bridges the app-layer present diagnostics onto the engine's live snapshot.
//
// The engine never measures presentation: `CaptureDiagnostics::source_present_mode`,
// `source_tearing` and `present_mode_availability` are declared in recorder_core so the
// snapshot has ONE shape, but the only producer able to fill them is the elevation- and
// opt-in-gated PresentMon ETW consumer, which lives in the app layer. Until Wave D
// nothing ever wrote them, so `pipeline.snapshot` reported `presentMode: null` on every
// machine and the capture-stall classifier's exclusive-fullscreen refinement was
// unreachable code rather than a check that happened not to fire.
//
// Pure, so the mapping is testable without ETW, elevation or a recording.

// Mirror of app::diagnostics::PresentMode onto the recorder_core enum. The two are
// distinct types with identical members, deliberately: recorder_core must not include
// an app-layer header.
[[nodiscard]] recorder_core::PresentMode ToSnapshotPresentMode(PresentMode mode) noexcept;

// Overlays `sample` onto `capture`. Availability is granted ONLY when the provider is
// active AND a real present has been observed AND its mode has been classified:
//
//   - no sample (provider absent, opt-in off, not elevated, session not open) -> untouched
//   - sample present but `available == false`                                 -> untouched
//   - sample available but mode still Unknown (no present seen yet)           -> untouched
//
// "Untouched" means the snapshot keeps `Unavailable`, which the surfaces render as an
// em dash. A fabricated `Composed` / `tearing: false` would be indistinguishable from a
// measured one, and the whole point of the availability ladder is that it is not.
void ApplyPresentSample(recorder_core::CaptureDiagnostics& capture, const std::optional<PresentSample>& sample);

} // namespace exosnap::diagnostics
