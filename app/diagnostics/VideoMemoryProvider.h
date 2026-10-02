#pragma once

#include "MeasurementMetadata.h"
#include <array>

namespace exosnap::diagnostics {

struct VideoMemoryBudget {
    uint64_t current_usage_bytes = 0;
    uint64_t budget_bytes = 0;
    uint64_t reservation_bytes = 0;
    uint64_t available_for_reservation_bytes = 0;
    [[nodiscard]] uint64_t headroom_bytes() const noexcept {
        return budget_bytes > current_usage_bytes ? budget_bytes - current_usage_bytes : 0;
    }
};

struct LogicalSurfaceUsage {
    const char* owner = nullptr;
    uint64_t bytes = 0;
    uint64_t surfaces = 0;
};

struct VideoMemoryReading {
    // Sampled across all adapters. Logical owned texels, not DXGI residency.
    std::array<LogicalSurfaceUsage, 9> logical_surfaces{};
    bool logical_surfaces_sampled = false;
    MeasurementMetadata metadata;
    // Process usage on the attributed adapter, not device-wide VRAM use.
    std::optional<VideoMemoryBudget> local;
    std::optional<VideoMemoryBudget> nonlocal;
};

class IVideoMemoryProvider {
  public:
    virtual ~IVideoMemoryProvider() = default;
    virtual VideoMemoryReading Read(int64_t adapter_luid) = 0;
};

} // namespace exosnap::diagnostics
