#pragma once

#include <dxgicommon.h>

namespace exosnap::engine {

// Whether a display's reported colour space means "HDR is on right now".
//
// Windows reports an HDR desktop as PQ (SMPTE 2084) over BT.2020, in either full
// or studio (limited) range. Both mean HDR; the range says how the display encodes
// levels, not whether HDR is engaged.
//
// This lives here because the answer must be identical everywhere. Capability
// probing and the capture path used to disagree — the capture path accepted only
// the full-range variant — so a studio-range HDR display made the coordinator pin
// PQ/BT.2020 metadata while the capture path saw SDR, and the session died on the
// resulting mismatch.
constexpr bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE color_space) noexcept {
    return color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
           color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

} // namespace exosnap::engine
