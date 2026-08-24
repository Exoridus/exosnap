#pragma once

// Single definition of the scRGB reference-white luminance, shared by the HDR
// tone-map (hdr_tonemap.h) and the HDR10 PQ (hdr_pq.h) reference math so both —
// and the monitoring chain that combines them (hdr_preview.h) — agree on one
// value without a redefinition clash when included together.

namespace exosnap::engine {

// scRGB reference white: a channel value of 1.0 corresponds to this luminance
// (SDR reference white, 80 cd/m^2).
inline constexpr float kHdrReferenceWhiteNits = 80.0f;

} // namespace exosnap::engine
