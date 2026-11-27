#pragma once

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
#include "Handle.h"
#include <windows.h>

#include <atomic>
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
inline std::atomic<bool> fixture_footprint_mismatch{false};
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

inline void NoteFootprintMismatch() {
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
    fixture_footprint_mismatch.store(true);
    ReportFixtureStatus("installed footprint differs from verified MSIX", ERROR_INVALID_DATA);
#endif
}

} // namespace exosnap::diagnostics::ipc

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
namespace exosnap::diagnostics {
bool ProbeSignedPackageAcquisition();
HRESULT ProbeMachineBootstrap(ULONG version, bool invoke);
} // namespace exosnap::diagnostics
#endif
