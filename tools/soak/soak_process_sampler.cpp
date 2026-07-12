#include "soak_process_sampler.h"

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

    m.gdi_objects = GetGuiResources(self, GR_GDIOBJECTS);
    m.user_objects = GetGuiResources(self, GR_USEROBJECTS);
    return m;
}

} // namespace exosnap::soak
