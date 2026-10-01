#pragma once

#include "MeasurementMetadata.h"

#include <cstdint>
#include <vector>

namespace exosnap::diagnostics {

// Presentation mode of the captured source as reported by the present-diagnostics
// provider (PresentMon ETW). Composed == DWM-composited window;
// IndependentFlip == flip-model presentation (overlay plane); ExclusiveFullscreen ==
// legacy exclusive fullscreen. Unknown until a present is observed.
enum class PresentMode {
    Unknown,
    Composed,
    IndependentFlip,
    ExclusiveFullscreen,
};

// Optional presentation enrichment. Unavailable observations remain neutral.
// Consumers do not depend on the transport (direct ETW or a service).
struct PresentSample {
    MeasurementMetadata metadata;
    PresentMode mode = PresentMode::Unknown;
    bool tearing = false;
    double present_interval_ms = 0.0;
    bool available = false;
    // True when the sample was filtered to the captured process. Display and
    // Region capture have no process to attribute to, so their samples span the
    // whole desktop and the per-source checks (discarded presents, mode flips)
    // must not read them as a statement about the recorded content.
    bool attributed = false;

    // Session-cumulative aggregates, accumulated by PresentMonEtwSession across the drain
    // (NOT per-event — the drain otherwise keeps only the latest present). `present_count`
    // gates the discarded-ratio check against warm-up noise.
    uint32_t present_count = 0;   // total matched presents observed this session
    uint32_t discarded_count = 0; // presents the compositor discarded (FinalState == Discarded)
    uint32_t mode_flip_count = 0; // classified present-mode transitions (instability proxy)

    // Raw attribution evidence for live verification. These fields explain an
    // unavailable PID-filtered sample without changing the user-facing verdict.
    uint32_t trace_drained_count = 0;
    uint32_t trace_matched_count = 0;
    unsigned long trace_last_process_id = 0;
    uint64_t trace_last_hwnd = 0;
    std::vector<unsigned long> trace_process_ids;
    std::vector<unsigned long> trace_related_process_ids;
};

// Presentation measurements, independent of the provider transport and privilege
// requirements. Core recording health uses engine-owned outcome measurements.
class IPresentProvider {
  public:
    virtual ~IPresentProvider() = default;

    // Latest present observation. When the provider is not active (see
    // IsAvailable) this returns a default sample with available == false.
    [[nodiscard]] virtual PresentSample Sample() const = 0;

    // Whether the provider can produce observations. Each sample separately
    // declares whether it contains a measured, attributed value.
    [[nodiscard]] virtual bool IsAvailable() const = 0;
};

} // namespace exosnap::diagnostics
