#pragma once

// Host process metrics for the soak timeline. RAM/handles/GUI objects are
// properties of the HOST process, never the engine — so they live here, in the
// tool, behind an interface the tests can substitute.

#include <cstdint>

namespace exosnap::soak {

struct ProcessMetrics {
    uint64_t rss_bytes = 0;     // WorkingSetSize
    uint64_t private_bytes = 0; // PrivateUsage (commit charge)
    uint32_t handle_count = 0;
    uint32_t gdi_objects = 0;
    uint32_t user_objects = 0;
};

class IProcessSampler {
  public:
    virtual ~IProcessSampler() = default;
    virtual ProcessMetrics Sample() = 0;
};

// Real sampler: psapi GetProcessMemoryInfo (PROCESS_MEMORY_COUNTERS_EX) +
// GetProcessHandleCount + GetGuiResources on the current process. WinAPI-only;
// exercised live (there is no CI test for it — a fake stands in for CI).
class WinProcessSampler : public IProcessSampler {
  public:
    ProcessMetrics Sample() override;
};

} // namespace exosnap::soak
