#pragma once

#include "GpuTelemetryProvider.h"

#include <span>

namespace exosnap::diagnostics {

// Read-only subset of the NVML C ABI, loaded from the installed driver.
// No driver settings entry points are resolved or exposed.
struct NvmlReadApi {
    using Device = void*;
    struct Utilization {
        unsigned gpu;
        unsigned memory;
    };
    int (*utilization)(Device, Utilization*) = nullptr;
    int (*encoder_utilization)(Device, unsigned*, unsigned*) = nullptr;
    int (*encoder_stats)(Device, unsigned*, unsigned*, unsigned*) = nullptr;
    int (*temperature)(Device, unsigned, unsigned*) = nullptr;
    int (*clock)(Device, unsigned, unsigned*) = nullptr;
    int (*performance_state)(Device, unsigned*) = nullptr;
};

struct NvidiaAdapterIdentity {
    uint32_t device_id = 0;
    uint32_t subsystem_id = 0;
};

// Returns an index only for a unique identity match. Identical GPUs require
// stronger attribution and deliberately remain unavailable through this path.
std::optional<size_t> MatchNvidiaAdapter(NvidiaAdapterIdentity target,
                                         std::span<const NvidiaAdapterIdentity> devices) noexcept;
GpuTelemetryReading ReadNvmlMeasurements(const NvmlReadApi& api, NvmlReadApi::Device device, int64_t luid);

class NvidiaGpuTelemetryProvider final : public IGpuTelemetryProvider {
  public:
    GpuTelemetryReading Read(int64_t adapter_luid) override;
};

} // namespace exosnap::diagnostics
