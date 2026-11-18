#include "soak_process_sampler.h"

#include <exosnap/engine/recorder_session.h>
#include <limits>

#include "diagnostics/DxgiVideoMemoryProvider.h"
#include "diagnostics/NvidiaGpuTelemetryProvider.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>

namespace exosnap::soak {

ProcessMetrics WinProcessSampler::Sample() {
    ProcessMetrics m;
    const HANDLE self = GetCurrentProcess();

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.rss_bytes = static_cast<uint64_t>(pmc.WorkingSetSize);
        m.private_bytes = static_cast<uint64_t>(pmc.PrivateUsage);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(self, &handles))
        m.handle_count = static_cast<uint32_t>(handles);

    FILETIME created{}, exited{}, process_kernel{}, process_user{}, idle{}, system_kernel{}, system_user{};
    if (GetProcessTimes(self, &created, &exited, &process_kernel, &process_user) &&
        GetSystemTimes(&idle, &system_kernel, &system_user)) {
        const auto ticks = [](const FILETIME& value) {
            return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        const CpuCounters current{ticks(process_kernel) + ticks(process_user),
                                  ticks(system_kernel) + ticks(system_user), ticks(idle)};
        if (previous_cpu_ && current.system > previous_cpu_->system && current.process >= previous_cpu_->process &&
            current.idle >= previous_cpu_->idle) {
            const uint64_t capacity = current.system - previous_cpu_->system;
            const uint64_t idle_delta = current.idle - previous_cpu_->idle;
            const uint64_t process_delta = current.process - previous_cpu_->process;
            if (idle_delta <= capacity)
                m.system_cpu_percent =
                    100.0 * static_cast<double>(capacity - idle_delta) / static_cast<double>(capacity);
            if (process_delta <= capacity)
                m.process_cpu_percent = 100.0 * static_cast<double>(process_delta) / static_cast<double>(capacity);
        }
        previous_cpu_ = current;
    }
    m.gdi_objects = GetGuiResources(self, GR_GDIOBJECTS);
    m.user_objects = GetGuiResources(self, GR_USEROBJECTS);
    return m;
}

StorageObservation ReadSoakStorage(const std::filesystem::path& requested_output, exosnap::engine::Container container,
                                   const std::filesystem::path& system_volume, double elapsed) {
    const auto output = container == exosnap::engine::Container::Mp4
                            ? exosnap::engine::DeriveTransientMkvPath(requested_output)
                            : requested_output;
    StorageObservation observation;
    observation.t_s = elapsed;
    std::error_code error;
    const auto output_space = std::filesystem::space(output.parent_path(), error);
    if (!error && output_space.available != std::numeric_limits<uint64_t>::max())
        observation.output_free_bytes = output_space.available;
    const auto system_space = std::filesystem::space(system_volume, error);
    if (!error && system_space.available != std::numeric_limits<uint64_t>::max())
        observation.system_free_bytes = system_space.available;
    const bool exists = std::filesystem::exists(output, error);
    if (!error) {
        if (exists) {
            const auto size = std::filesystem::file_size(output, error);
            if (!error)
                observation.file_bytes = size;
        } else {
            observation.file_bytes = 0;
        }
    }
    return observation;
}

ProcessMetrics WinProcessSampler::SampleForAdapter(int64_t adapter_luid) {
    auto metrics = Sample();
    if (adapter_luid != 0) {
        exosnap::diagnostics::NvidiaGpuTelemetryProvider gpu;
        const auto reading = gpu.Read(adapter_luid);
        metrics.gpu_utilization_percent = reading.utilization_percent;
        metrics.encoder_utilization_percent = reading.encoder_utilization_percent;
        exosnap::diagnostics::DxgiVideoMemoryProvider memory;
        const auto residency = memory.Read(adapter_luid);
        if (residency.local)
            metrics.process_gpu_memory_bytes = residency.local->current_usage_bytes;
    }
    return metrics;
}

} // namespace exosnap::soak
