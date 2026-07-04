#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure, GPU-independent HDR10 (PQ / BT.2020) output math. The compute path
// (gpu_hdr_pq.*) replicates these exact formulas in HLSL so the CPU reference
// and the shader agree; this header is unit-tested with golden values and has
// no D3D dependency (safe to include from tests).
//
// Native HDR10 capture model: a monitor duplicated while the desktop is in
// HDR/Advanced-Color mode is delivered as scRGB FP16 — linear light, BT.709
// primaries, where a channel value of 1.0 equals SDR reference white
// (80 cd/m^2). The native path keeps the HDR signal instead of tone-mapping it:
//
//   scRGB linear (BT.709)                          [capture]
//     -> nit scaling  (1.0 -> 80 nits, normalised to the 10 000-nit PQ range)
//     -> BT.709 -> BT.2020 gamut matrix (linear light)
//     -> PQ OETF (SMPTE ST 2084) per channel        -> non-linear R'G'B'
//     -> R'G'B' -> Y'CbCr BT.2020 non-constant-luminance, limited range
//     -> 10-bit codes packed into P010.
//
// There is no tone-mapping here: the signal is passed through to the display
// peak reality of the encoded PQ curve. Values above 10 000 nits (the PQ
// ceiling) are clamped, never rolled off.
//
// A rarer capture variant delivers an HDR10 desktop already PQ/BT.2020-encoded
// as R10G10B10A2 (non-linear R'G'B'). That path skips the nit-scale / gamut /
// PQ stages entirely and only applies the Y'CbCr conversion + packing
// (PqRgbToYcbcr / QuantizeYcbcr10Limited below).

namespace recorder_core {

// scRGB reference white: channel value 1.0 corresponds to this luminance.
inline constexpr float kHdrReferenceWhiteNits = 80.0f;

// PQ (SMPTE ST 2084) encodes absolute luminance up to this ceiling; 1.0 on the
// PQ curve is 10 000 cd/m^2. Content brighter than this is clamped.
inline constexpr float kPqPeakNits = 10000.0f;

// SMPTE ST 2084 (PQ) constants.
inline constexpr float kPqM1 = 2610.0f / 16384.0f;         // 0.1593017578125
inline constexpr float kPqM2 = 2523.0f / 4096.0f * 128.0f; // 78.84375
inline constexpr float kPqC1 = 3424.0f / 4096.0f;          // 0.8359375
inline constexpr float kPqC2 = 2413.0f / 4096.0f * 32.0f;  // 18.8515625
inline constexpr float kPqC3 = 2392.0f / 4096.0f * 32.0f;  // 18.6875

// PQ opto-electronic transfer function (ST 2084, "inverse EOTF"): normalised
// linear luminance L in [0, 1] (1.0 = 10 000 cd/m^2) -> non-linear signal in
// [0, 1]. Clamps its input to the valid range.
inline float PqOetf(float linear_normalized) {
    float l = linear_normalized;
    if (l < 0.0f) {
        l = 0.0f;
    }
    if (l > 1.0f) {
        l = 1.0f;
    }
    const float lm1 = std::pow(l, kPqM1);
    return std::pow((kPqC1 + kPqC2 * lm1) / (1.0f + kPqC3 * lm1), kPqM2);
}

// scRGB linear channel value (1.0 = reference white = 80 cd/m^2) -> normalised
// PQ input in [0, 1] (fraction of the 10 000-nit ceiling). Negatives (wide-gamut
// scRGB can go slightly negative) clamp to 0; values above 10 000 nits clamp to
// 1.0 (PQ passthrough, no roll-off).
inline float ScrgbToPqNormalized(float scrgb_linear) {
    float nits = scrgb_linear * kHdrReferenceWhiteNits;
    if (nits < 0.0f) {
        nits = 0.0f;
    }
    float l = nits / kPqPeakNits;
    if (l > 1.0f) {
        l = 1.0f;
    }
    return l;
}

struct LinearRgb {
    float r;
    float g;
    float b;
};

// BT.709 -> BT.2020 gamut conversion in linear light (Rec. BT.2087 primaries
// matrix). Applied before the PQ curve.
inline LinearRgb Bt709ToBt2020(const LinearRgb& c) {
    return LinearRgb{
        0.6274038959f * c.r + 0.3292830384f * c.g + 0.0433130657f * c.b,
        0.0690972894f * c.r + 0.9195403951f * c.g + 0.0113623156f * c.b,
        0.0163914389f * c.r + 0.0880133079f * c.g + 0.8955952532f * c.b,
    };
}

struct Ycbcr {
    float y;  // [0, 1]
    float cb; // [-0.5, 0.5]
    float cr; // [-0.5, 0.5]
};

// BT.2020 non-constant-luminance luma coefficients (Rec. ITU-R BT.2020-2).
inline constexpr float kKr2020 = 0.2627f;
inline constexpr float kKb2020 = 0.0593f;
inline constexpr float kKg2020 = 1.0f - kKr2020 - kKb2020; // 0.6780

// Non-linear R'G'B' (PQ-encoded, [0, 1]) -> Y'CbCr BT.2020 NCL signal. The
// primed inputs are already gamma/PQ-encoded (non-constant luminance derives
// luma from the non-linear signal). Cb/Cr are returned in [-0.5, 0.5].
inline Ycbcr PqRgbToYcbcr(float rp, float gp, float bp) {
    const float y = kKr2020 * rp + kKg2020 * gp + kKb2020 * bp;
    return Ycbcr{
        y,
        (bp - y) / (2.0f * (1.0f - kKb2020)),
        (rp - y) / (2.0f * (1.0f - kKr2020)),
    };
}

struct P010Codes {
    uint16_t y;
    uint16_t cb;
    uint16_t cr;
};

inline uint16_t ClampCode10(float v) {
    float r = std::floor(v + 0.5f);
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (r > 1023.0f) {
        r = 1023.0f;
    }
    return static_cast<uint16_t>(r);
}

// Quantise a Y'CbCr signal to 10-bit limited ("studio") range codes:
// luma 64..940, chroma 64..960 centred on 512 (Rec. ITU-R BT.2020 / general
// n-bit narrow-range quantisation with n = 10).
inline P010Codes QuantizeYcbcr10Limited(const Ycbcr& c) {
    return P010Codes{
        ClampCode10(c.y * 876.0f + 64.0f),
        ClampCode10(c.cb * 896.0f + 512.0f),
        ClampCode10(c.cr * 896.0f + 512.0f),
    };
}

// Full scRGB(HDR, linear BT.709) -> HDR10 P010 code chain for one pixel.
inline P010Codes ScrgbToP010(const LinearRgb& scrgb) {
    const LinearRgb lin2020 = Bt709ToBt2020(LinearRgb{
        ScrgbToPqNormalized(scrgb.r),
        ScrgbToPqNormalized(scrgb.g),
        ScrgbToPqNormalized(scrgb.b),
    });
    const float rp = PqOetf(lin2020.r);
    const float gp = PqOetf(lin2020.g);
    const float bp = PqOetf(lin2020.b);
    return QuantizeYcbcr10Limited(PqRgbToYcbcr(rp, gp, bp));
}

// Already-PQ/BT.2020 R'G'B' (e.g. an HDR10 R10G10B10A2 desktop) -> P010 codes.
// Only the Y'CbCr conversion + narrow-range packing is applied; the signal is
// not re-transferred (input is already PQ-encoded).
inline P010Codes PqRgbToP010(float rp, float gp, float bp) {
    return QuantizeYcbcr10Limited(PqRgbToYcbcr(rp, gp, bp));
}

} // namespace recorder_core
