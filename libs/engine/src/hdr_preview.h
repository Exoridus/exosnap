#pragma once

// On-screen monitoring decode for a native HDR10 (PQ / BT.2020 / P010) session.
//
// The encode path keeps the captured HDR signal and packs it into P010 as
// PQ-encoded BT.2020 Y'CbCr (see hdr_pq.h). The in-app monitoring surfaces (the
// recording-view live preview tap and the frame snapshot) read that same P010
// back on the CPU. Decoding it with the ordinary SDR BT.709 YUV->BGRA matrix
// treats the PQ signal as if it were linear/SDR, which looks washed out and flat
// (reference white lands around mid-grey). This chain instead inverts the PQ /
// BT.2020 encoding and tone-maps the result to SDR BT.709 for display:
//
//   P010 limited-range Y'CbCr (BT.2020 NCL)   [monitoring readback]
//     -> dequantise + inverse Y'CbCr -> R'G'B' (PQ non-linear)
//     -> PQ EOTF per channel               -> normalised linear (1.0 = 10 000 nits)
//     -> BT.2020 -> BT.709 gamut (linear light)
//     -> scale to scRGB reference-white multiples (1.0 = 80 nits)
//     -> the shared HDR->SDR tone-map operator (hdr_tonemap.h) at the session's
//        display peak, then the sRGB OETF                   -> SDR BGRA8.
//
// This is a monitoring approximation (product decision): the tone-map curve and
// the PQ / gamut math are the same reference helpers the capture paths use, not a
// second invented curve. No D3D dependency: pure math over caller-supplied mapped
// buffers, unit-testable without a device.

#include "hdr_pq.h"
#include "hdr_tonemap.h"
#include "yuv_to_bgra.h"

#include <array>
#include <cstdint>

namespace exosnap::engine {

// scRGB reference-white multiples per unit of normalised PQ linear luminance
// (1.0 PQ linear = 10 000 nits; 1.0 scRGB = 80 nits).
inline constexpr float kPqLinearToScrgb = kPqPeakNits / kHdrReferenceWhiteNits;

// One normalised, PQ-decoded, gamut-mapped BT.709 linear channel value ->
// tone-mapped SDR non-linear signal in [0, 1]. Bridges the PQ luminance scale
// (1.0 = 10 000 nits) to the tone-map operator's scRGB scale (1.0 = 80 nits) and
// reuses the exact operator + OETF from the capture tone-map path.
inline float Bt709LinearToSdrChannel(float linear_normalized, float peak_scale) {
    return ScrgbToSdr709Channel(linear_normalized * kPqLinearToScrgb, peak_scale);
}

struct MonitorBgr {
    uint8_t b;
    uint8_t g;
    uint8_t r;
};

// Full reference chain for one P010 pixel (10-bit limited-range BT.2020 PQ
// Y'CbCr codes) -> approximate SDR BT.709 BGR for on-screen monitoring, tone-
// mapped for a display of the given peak (reference-white multiples, >= 1.0).
inline MonitorBgr P010PqPixelToMonitorBgr(uint16_t y10, uint16_t cb10, uint16_t cr10, float peak_scale) {
    const Ycbcr yc{DequantY10Limited(y10), DequantC10Limited(cb10), DequantC10Limited(cr10)};
    float rp = 0.0f;
    float gp = 0.0f;
    float bp = 0.0f;
    YcbcrToPqRgb(yc, rp, gp, bp);

    const LinearRgb lin2020{PqEotf(rp), PqEotf(gp), PqEotf(bp)};
    const LinearRgb lin709 = Bt2020ToBt709(lin2020);

    auto to_byte = [peak_scale](float linear_normalized) -> uint8_t {
        const float sdr = Bt709LinearToSdrChannel(linear_normalized, peak_scale);
        float v = sdr * 255.0f + 0.5f;
        if (v < 0.0f) {
            v = 0.0f;
        }
        if (v > 255.0f) {
            v = 255.0f;
        }
        return static_cast<uint8_t>(v);
    };
    return MonitorBgr{to_byte(lin709.b), to_byte(lin709.g), to_byte(lin709.r)};
}

// Converts native-HDR10 P010 frames (PQ / BT.2020, 10-bit limited range) to
// top-down BGRA8888 (B, G, R, A; alpha always 255) tone-mapped to SDR BT.709 for
// on-screen monitoring. peak_scale is the session display peak in reference-white
// multiples (HdrPeakScale, >= 1.0) — session-constant, so the per-channel
// transfer stages are baked into lookup tables once at construction and reused
// for every frame (the per-pixel PQ EOTF / tone-map / OETF are otherwise
// pow()-bound and too slow at 4K on the preview thread). Convert() matches
// P010PqPixelToMonitorBgr within table quantisation.
class P010PqMonitorConverter {
  public:
    // Samples the transfer tables from the reference chain above for the given
    // session display peak.
    explicit P010PqMonitorConverter(float peak_scale);

    // out_bgra must have at least height * out_stride_bytes bytes;
    // out_stride_bytes must be >= src.width * 4. Does nothing if src has zero
    // size, a null plane, or a null output. src.bits_per_sample is assumed 10
    // (P010).
    void Convert(const PlanarYuv420Frame& src, uint8_t* out_bgra, uint32_t out_stride_bytes) const;

    // Same conversion for the fully-planar 10-bit layout FFmpeg's software
    // decoders produce (AV_PIX_FMT_YUV420P10LE: separate U and V planes, plain
    // [0, 1023] samples with no P010 <<6 justification) -- what the editor's
    // player hands over for a natively-HDR10 recording. Repacking to P010 to
    // reuse the overload above would cost a full extra frame copy on a path
    // that is already the editor's measured bottleneck.
    void Convert(const FullPlanarYuv420Frame& src, uint8_t* out_bgra, uint32_t out_stride_bytes) const;

  private:
    // One output pixel from PQ-encoded Y'CbCr components that have already been
    // dequantised to their nominal ranges. Both overloads go through this, so
    // the two input layouts cannot drift apart in colour.
    void WriteMonitorPixel(float yv, float cr_r, float cb_b, float g_chroma, uint8_t* px) const;

    // Table resolution for the per-channel transfer stages. 1024 entries is
    // ample for an 8-bit monitoring output and keeps both tables in L1/L2.
    static constexpr int kLutSize = 1024;

    // PQ signal [0, 1] -> normalised linear PQ luminance [0, 1] (PqEotf).
    std::array<float, kLutSize> eotf_lut_{};
    // BT.709 linear (normalised, clamped [0, 1]) -> tone-mapped SDR byte.
    std::array<uint8_t, kLutSize> sdr_lut_{};
};

} // namespace exosnap::engine
