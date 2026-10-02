#pragma once

#include "MeasurementMetadata.h"

namespace exosnap::diagnostics {

// Device-wide utilization is attribution evidence, never proof of recorder loss.
// Each field is optional independently because driver support varies.
struct GpuTelemetryReading {
    MeasurementMetadata metadata;
    std::optional<double> utilization_percent;
    std::optional<double> encoder_utilization_percent;
    std::optional<unsigned> encoder_sampling_period_us;
    std::optional<unsigned> encoder_sessions;
    std::optional<unsigned> encoder_average_fps;
    std::optional<unsigned> encoder_average_latency_us;
    std::optional<unsigned> temperature_celsius;
    std::optional<unsigned> graphics_clock_mhz;
    std::optional<unsigned> performance_state;
};

class IGpuTelemetryProvider {
  public:
    virtual ~IGpuTelemetryProvider() = default;
    // Poll away from recording workers. A missing/mismatched adapter returns
    // unavailable fields. Implementations must never change device settings.
    virtual GpuTelemetryReading Read(int64_t adapter_luid) = 0;
};

} // namespace exosnap::diagnostics
