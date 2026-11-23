#pragma once

#include "PresentProvider.h"

#include <chrono>
#include <optional>

#include <exosnap/engine/pipeline_diagnostics.h>

namespace exosnap::diagnostics {

[[nodiscard]] exosnap::engine::PresentMode ToSnapshotPresentMode(PresentMode mode) noexcept;

// Publishes source presentation only for a classified, fresh observation of the selected process.
void ApplyPresentSample(exosnap::engine::CaptureDiagnostics& capture, const std::optional<PresentSample>& sample,
                        unsigned long target_pid, std::chrono::steady_clock::time_point now);

} // namespace exosnap::diagnostics
