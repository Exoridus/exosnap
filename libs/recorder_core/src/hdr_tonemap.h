#pragma once

#include <algorithm>
#include <cmath>

// Pure, GPU-independent HDR-to-SDR tone-map math. The compute path
// (gpu_hdr_tonemap.*) replicates these exact formulas in HLSL so the CPU
// reference and the shader agree bit-for-bit; this header is unit-tested with
// golden values. No D3D dependency — safe to include from tests.
//
// Capture model: a monitor duplicated while the desktop is in HDR/Advanced-Color
// mode is delivered as scRGB FP16 — linear light, BT.709 primaries, where a
// channel value of 1.0 equals SDR reference white (80 cd/m^2). Because the
// primaries already match BT.709, no gamut matrix is needed: mapping to an SDR
// BT.709 signal is a luminance roll-off followed by the BT.709 OETF.

namespace recorder_core {

// scRGB reference white: channel value 1.0 corresponds to this luminance.
inline constexpr float kHdrReferenceWhiteNits = 80.0f;

// Fallback display peak used when the capture display's active peak luminance is
// unknown (the display is not reporting an active HDR colour space). 1000 cd/m^2
// is a common consumer-HDR peak and errs toward gentler highlight compression.
inline constexpr float kHdrFallbackPeakNits = 1000.0f;

// Knee point in reference-white multiples. Content at or below reference white
// (shadows, midtones, paper white) is preserved unchanged; only the highlight
// range above the knee is rolled off.
inline constexpr float kHdrToneMapKnee = 0.80f;

// Peak luminance, expressed in reference-white multiples, that maps to output
// 1.0. The display's reported luminance is only trusted when the display is
// actively in an HDR colour space: a display in SDR mode still reports its EDID
// luminance caps (measured: 1499 cd/m^2 on an SDR panel), which must not drive
// the knee. Result is always >= 1.0.
inline float HdrPeakScale(bool display_hdr_active, float display_max_luminance_nits) {
    float peak_nits = kHdrFallbackPeakNits;
    if (display_hdr_active && display_max_luminance_nits > kHdrReferenceWhiteNits) {
        peak_nits = display_max_luminance_nits;
    }
    return peak_nits / kHdrReferenceWhiteNits; // >= 1.0
}

// Highlight roll-off for one linear scRGB channel. Returns SDR linear in [0, 1].
// Identity at/below the knee; the [knee, peak] range is compressed onto
// [knee, 1.0], reaching 1.0 exactly at peak. Monotonic, never exceeds 1.0.
inline float HdrToneMapChannel(float scrgb_linear, float peak_scale) {
    const float x = scrgb_linear > 0.0f ? scrgb_linear : 0.0f; // clamp wide-gamut negatives
    const float knee = kHdrToneMapKnee;
    if (x <= knee || peak_scale <= knee) {
        return x < 1.0f ? x : 1.0f;
    }
    const float head = 1.0f - knee;             // output headroom above the knee
    const float max_excess = peak_scale - knee; // input excess mapped onto the headroom
    const float e = x - knee;                   // this sample's excess above the knee
    // Extended-Reinhard shoulder on the excess: s(0)=0, s(max_excess)=1,
    // monotonic. y reaches 1.0 exactly at x == peak_scale and clamps beyond it.
    const float s = e * (1.0f + e / (max_excess * max_excess)) / (1.0f + e);
    const float y = knee + head * s;
    return y < 1.0f ? y : 1.0f;
}

// BT.709 opto-electronic transfer function (Rec.709 s.1.2): SDR linear [0, 1] ->
// non-linear signal [0, 1].
inline float Bt709Oetf(float linear) {
    float l = linear;
    if (l < 0.0f) {
        l = 0.0f;
    }
    if (l > 1.0f) {
        l = 1.0f;
    }
    return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}

// Full per-channel scRGB (HDR, linear) -> BT.709 SDR non-linear signal.
inline float ScrgbToSdr709Channel(float scrgb_linear, float peak_scale) {
    return Bt709Oetf(HdrToneMapChannel(scrgb_linear, peak_scale));
}

} // namespace recorder_core
