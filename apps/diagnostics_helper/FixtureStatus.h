#pragma once

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
#include <windows.h>

#include <atomic>
#include <cstdio>
#endif

namespace exosnap::diagnostics::ipc {

inline void ReportFixtureStatus([[maybe_unused]] const char* stage, [[maybe_unused]] unsigned long status) {
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
    std::fprintf(stderr, "Diagnostics fixture %s: %lu (0x%08lx)\n", stage, status, status);
#endif
}

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
inline std::atomic<bool> fixture_footprint_mismatch{false};
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
