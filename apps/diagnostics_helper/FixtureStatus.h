#pragma once

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
#include "Handle.h"
#include <windows.h>

#include <cstdio>
#include <mutex>
#endif

namespace exosnap::diagnostics::ipc {

inline void ReportFixtureStatus([[maybe_unused]] const char* stage, [[maybe_unused]] unsigned long status) {
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
    std::fprintf(stderr, "Diagnostics fixture %s: %lu (0x%08lx)\n", stage, status, status);
#endif
}

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
// A fresh observer handle comes only from the authenticated kernel pipe peer.
inline std::mutex fixture_collector_mutex;
inline Handle fixture_collector;
inline void ObserveFixtureCollector(HANDLE process) {
    HANDLE copy = nullptr;
    DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &copy,
                    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
    std::lock_guard lock(fixture_collector_mutex);
    fixture_collector = Handle(copy);
}
inline Handle TakeFixtureCollector() {
    std::lock_guard lock(fixture_collector_mutex);
    return std::move(fixture_collector);
}

#endif

} // namespace exosnap::diagnostics::ipc

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
namespace exosnap::diagnostics {
HRESULT ProbeMachineBootstrap(ULONG version, bool invoke);
} // namespace exosnap::diagnostics
#endif
