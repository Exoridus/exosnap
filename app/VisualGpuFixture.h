#pragma once

#include <capability/capability_set.h>

namespace exosnap {

// Visual-test only: pin a deterministic GPU adapter name matching the Device
// page's fixture, so Diagnostics never renders the real machine adapter. Both
// fields are overridden because the Encoder tile reads gpu_adapter_name while the
// expert capability summary prefers runtime.nvidia.adapter_name.
//
// Shared rather than file-local: the scenario code applies it, and
// MainWindow::refreshDiagnosticsData() re-applies it afterwards, because the
// async caps-ready path re-assigns runtime_caps_ from the real probe once the
// scenario has already run. That second call site is NOT behind a harness guard
// — it is reached through the visual_diagnostics_gpu_override_ member — so this
// header must stay available in every configuration, Release included.
inline void ApplyVisualGpuFixture(capability::CapabilitySet& caps) {
    caps.gpu_adapter_name = "GeForce RTX 4070";
    caps.runtime.nvidia.adapter_name = "GeForce RTX 4070";
}

} // namespace exosnap
