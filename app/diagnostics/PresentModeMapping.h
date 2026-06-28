#pragma once

#include "PresentProvider.h"

namespace exosnap::diagnostics {

// A single completed present as observed by the PresentMon consumer, reduced to the
// few fields our diagnosis needs. Kept Win32/ETW-free so the mapping is unit-testable
// without the vendored lib. `present_mode_code` carries PresentMon's PresentMode enum
// integer; `sync_interval` is the DXGI present sync-interval (0 == tearing-capable).
struct RawPresentEvent {
    int present_mode_code = 0;
    int sync_interval = 1;
    bool tearing_flag = false;
    double interval_ms = 0.0;
    bool valid = false;
};

// Collapse a raw present into the diagnostics PresentSample. Invalid input yields a
// neutral Unavailable sample (never a fabricated Composed/zero).
[[nodiscard]] PresentSample MapPresentEvent(const RawPresentEvent& ev);

} // namespace exosnap::diagnostics
