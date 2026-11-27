#pragma once

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
#include <cstdio>
#endif

namespace exosnap::diagnostics::ipc {

inline void ReportFixtureStatus([[maybe_unused]] const char* stage, [[maybe_unused]] unsigned long status) {
#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
    std::fprintf(stderr, "Diagnostics fixture %s: %lu (0x%08lx)\n", stage, status, status);
#endif
}

} // namespace exosnap::diagnostics::ipc

#ifdef EXOSNAP_SIGNED_FIXTURE_STATUS
namespace exosnap::diagnostics {
void ProbeInstalledHelperLaunch();
}
#endif
