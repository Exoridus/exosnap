#pragma once

// Host process metrics for the soak timeline. RAM/handles/GUI objects are
// properties of the HOST process, never the engine — so they live here, in the
// tool, behind an interface the tests can substitute.

#include "soak_storage.h"

#include <cstdint>
#include <exosnap/engine/codec_types.h>
#include <filesystem>
#include <optional>

namespace exosnap::soak {

StorageObservation ReadSoakStorage(const std::filesystem::path& output, exosnap::engine::Container container,
                                   const std::filesystem::path& system_volume, double elapsed);

struct ProcessMetrics {
    uint64_t rss_bytes = 0;     // WorkingSetSize
    uint64_t private_bytes = 0; // PrivateUsage (commit charge)
    uint32_t handle_count = 0;
    uint32_t gdi_objects = 0;
    uint32_t user_objects = 0;
    std::optional<double> process_cpu_percent = std::nullopt;
    std::optional<double> system_cpu_percent = std::nullopt;
    std::optional<double> gpu_utilization_percent = std::nullopt;
    std::optional<double> encoder_utilization_percent = std::nullopt;
    std::optional<uint64_t> process_gpu_memory_bytes = std::nullopt;
};

class IProcessSampler {
  public:
    virtual ~IProcessSampler() = default;
    virtual ProcessMetrics Sample() = 0;
    virtual ProcessMetrics SampleForAdapter(int64_t) {
        return Sample();
    }
};

// Real sampler: Windows process memory, handles, CPU and attributed GPU telemetry.
class WinProcessSampler : public IProcessSampler {
  public:
    ProcessMetrics Sample() override;
    ProcessMetrics SampleForAdapter(int64_t adapter_luid) override;

  private:
    struct CpuCounters {
        uint64_t process;
        uint64_t system;
        uint64_t idle;
    };
    std::optional<CpuCounters> previous_cpu_;
};

} // namespace exosnap::soak
