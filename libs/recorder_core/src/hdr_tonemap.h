#pragma once

#include "hdr_reference_white.h"

#include <recorder_core/sdr_white_level.h>

#include <dxgiformat.h>

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
// signal is a luminance roll-off followed by the sRGB OETF.
//
// Why sRGB and not the BT.709 OETF, when the result is tagged BT.709: the two
// are different curves (mid-grey differs by 13/255) because BT.709's OETF is a
// camera curve whose paired display transfer is BT.1886's pure 2.4 gamma, giving
// a deliberate end-to-end system gamma of ~1.2 for a dim viewing environment.
// Neither end of this product's chain is that: the preview is composited by Qt
// as sRGB, the frame snapshot is written as an sRGB PNG, and desktop players
// decode BT.709-tagged SDR with an ~sRGB display transfer. Decisively, the plain
// SDR capture path applies no OETF at all -- the duplicated desktop is already
// sRGB-encoded and is passed through -- so encoding the HDR path with a camera
// curve would make one recording darker than the other under the same BT.709
// tag. The tag stays BT.709 (CICP 1, universally understood, and the primaries
// really are BT.709); only the transfer is sRGB throughout.

namespace recorder_core {

// scRGB reference white (kHdrReferenceWhiteNits) is defined in
// hdr_reference_white.h so this header and hdr_pq.h can be included together.

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

// sRGB opto-electronic transfer function: SDR linear [0, 1] -> non-linear signal
// [0, 1]. The single encode transfer for every SDR output this engine produces
// (see the transfer rationale at the top of this header). It is the exact
// inverse of the sRGB EOTF, so a desktop level that was never tone-mapped
// round-trips to its original code.
inline float SrgbOetf(float linear) {
    float l = linear;
    if (l < 0.0f) {
        l = 0.0f;
    }
    if (l > 1.0f) {
        l = 1.0f;
    }
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

// The OS SDR reference white, expressed in scRGB reference-white multiples.
//
// DOCUMENTED, and the reason the roll-off cannot work in raw scRGB units: on an
// HDR desktop Windows composes scene-referred, where scRGB 1.0 is 80 nits, and
// it renders SDR content at the user's SDR reference white level instead --
// "you can simply multiply the SDR color value by SdrWhiteLevelInNits / 80".
// So an sRGB mid-grey of 128 does NOT arrive as its own linear 0.2158; on a
// display set to 280 nits it arrives at 0.2158 * 3.5. Tone-mapping that without
// undoing the boost leaves it far above the knee's identity range and writes it
// out at 222 instead of 128 -- measured, and equally in the recording and the
// preview, because both apply this same transform.
//
// Returns >= 1.0. An unknown or implausible level resolves to the OS default
// through EffectiveOverlayReferenceWhiteNits, which is where those bounds live.
inline float SdrPaperWhiteScale(float sdr_white_level_nits) {
    const float nits = EffectiveOverlayReferenceWhiteNits(sdr_white_level_nits);
    const float scale = nits / kHdrReferenceWhiteNits;
    return scale > 1.0f ? scale : 1.0f;
}

// Full per-channel scRGB (HDR, linear) -> SDR non-linear signal, BT.709
// primaries with the sRGB transfer.
//
// `paper_white_scale` normalises the input so that SDR reference white lands on
// 1.0 before the roll-off; `peak_scale` is divided by the same factor so the
// knee keeps describing the same physical luminance. A scale of 1.0 is the
// scene-referred identity and reproduces the pre-normalisation behaviour.
inline float ScrgbToSdr709Channel(float scrgb_linear, float peak_scale, float paper_white_scale = 1.0f) {
    const float scale = paper_white_scale > 0.0f ? paper_white_scale : 1.0f;
    return SrgbOetf(HdrToneMapChannel(scrgb_linear / scale, peak_scale / scale));
}

// Full per-channel scRGB (SDR, linear) -> sRGB non-linear signal, for an
// Advanced-Color desktop that is still SDR (see OdCaptureMode::SdrScrgb). Same
// transfer as the HDR path; what differs is that there is no headroom to roll
// off, so reference white (1.0) must stay white and the roll-off is skipped.
inline float ScrgbSdrToSrgbChannel(float scrgb_linear) {
    return SrgbOetf(scrgb_linear);
}

// Pixel format for the HDR->SDR tone-map intermediate: the surface the tone-map
// shader renders the SDR BT.709 result into, and which the GPU compositor and
// VideoProcessor then consume. A 10-bit encode target (P010) gets an
// R10G10B10A2 intermediate so the extra depth survives the RGB->P010 conversion
// instead of being crushed at an 8-bit hop; an 8-bit encode keeps BGRA8. This is
// the pure format choice only — runtime device-capability fallback (a driver
// that rejects R10G10B10A2 for the render target or VideoProcessor input) is the
// call site's responsibility.
inline DXGI_FORMAT ToneMapIntermediateFormat(bool encode_is_10bit) {
    return encode_is_10bit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
}

} // namespace recorder_core
