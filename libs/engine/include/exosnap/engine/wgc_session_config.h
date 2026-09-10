#pragma once

#include <cstdint>

#include <windows.h>

#include <winrt/Windows.Foundation.h>

namespace exosnap::engine {

struct WgcMinUpdateIntervalResult {
    bool supported = false;
    int64_t requested_ticks = 0;
    int64_t effective_ticks = 0;
};

// Requests a 1 ms WGC update interval and reads back the value Windows accepted.
// An absent IGraphicsCaptureSession5 is the only successful fallback; every
// other query, write, or read error is returned to the caller.
[[nodiscard]] HRESULT ConfigureWgcMinUpdateInterval(const winrt::Windows::Foundation::IInspectable& session,
                                                    WgcMinUpdateIntervalResult* out) noexcept;

} // namespace exosnap::engine
