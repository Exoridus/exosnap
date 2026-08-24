#pragma once
#include <cstdint>

#include <winerror.h>

namespace exosnap::engine {

// How a failed Windows.Graphics.Capture acquire must be treated.
//
// WGC reports every failure as a WinRT exception, so the HRESULT carried by that
// exception is the only signal a caller gets. Recording and preview act on a
// failure differently -- recording ends the session, preview holds its last
// frame -- but they must not draw different CONCLUSIONS from the same code, or
// the same physical event means two things in one product.
enum class WgcAcquireFailure : uint8_t {
    // The D3D device behind the capture is gone. Nothing downstream recovers
    // without a new device, so retrying the same pool is pointless.
    DeviceLost,
    // The capture was interrupted or revoked while the device stayed usable:
    // a display-mode change, the secure desktop, or the item's own object
    // server going away.
    SourceLost,
    // Not a documented capture failure. Folding this into a known class is what
    // turns a real defect into an invisible one, so it is classified apart and
    // callers must surface it.
    Unexpected,
};

// `hr` is the HRESULT from winrt::hresult_error::code().
[[nodiscard]] constexpr WgcAcquireFailure ClassifyWgcAcquireFailure(int32_t hr) noexcept {
    switch (hr) {
    case static_cast<int32_t>(DXGI_ERROR_DEVICE_REMOVED):
    case static_cast<int32_t>(DXGI_ERROR_DEVICE_RESET):
    case static_cast<int32_t>(DXGI_ERROR_DEVICE_HUNG):
        return WgcAcquireFailure::DeviceLost;
    case static_cast<int32_t>(DXGI_ERROR_ACCESS_LOST):
    case static_cast<int32_t>(DXGI_ERROR_ACCESS_DENIED):
    case static_cast<int32_t>(RPC_E_DISCONNECTED):
        return WgcAcquireFailure::SourceLost;
    default:
        return WgcAcquireFailure::Unexpected;
    }
}

} // namespace exosnap::engine
