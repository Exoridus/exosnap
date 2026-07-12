#pragma once

#include <functional>
#include <vector>

#include "services/DisplayIdentityResolver.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// DisplayIdentityEnumerator
//
// The IMPURE half of the stable-display-identity resolver: it queries Win32
// (EnumDisplayMonitors + DisplayConfig) and Qt (best-effort EDID serial) to
// produce a StableDisplayId + physical anchor rect per active display. It lives
// in the app/services layer, never in the engine.
//
// The result feeds the pure ResolveStableDisplay matcher. Enumeration is
// comparatively expensive (QueryDisplayConfig + N x DisplayConfigGetDeviceInfo)
// and MUST NOT run from a hot path such as currentCapturePolicy(); callers cache
// the resolved identity at selection time instead.
// ---------------------------------------------------------------------------

// Production enumeration of the currently active displays. Never throws; on a
// DisplayConfig failure it degrades to gdi_name + physical rect only (so the
// matcher's stage-4 GDI fallback still works — never worse than the old match).
[[nodiscard]] std::vector<EnumeratedDisplayIdentity> EnumerateDisplayIdentities();

// Injectable seam so restore/re-resolve paths can be driven with a fixed
// identity list in tests without real hardware.
using DisplayIdentityEnumeratorFn = std::function<std::vector<EnumeratedDisplayIdentity>()>;

} // namespace exosnap
