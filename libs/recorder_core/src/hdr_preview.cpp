#include "hdr_preview.h"

#include <array>
#include <cstdint>

namespace recorder_core {

namespace {

// Table resolution for the per-channel transfer stages. 1024 entries is ample
// for an 8-bit monitoring output and keeps both tables in L1/L2.
constexpr int kLutSize = 1024;
constexpr float kLutMaxIndex = static_cast<float>(kLutSize - 1);

struct MonitorLuts {
    // PQ signal [0, 1] -> normalised linear PQ luminance [0, 1] (PqEotf).
    std::array<float, kLutSize> eotf{};
    // BT.709 linear (normalised, clamped [0, 1]) -> tone-mapped SDR byte.
    std::array<uint8_t, kLutSize> sdr{};
};

MonitorLuts BuildLuts(float peak_scale) {
    MonitorLuts luts;
    for (int i = 0; i < kLutSize; ++i) {
        const float t = static_cast<float>(i) / kLutMaxIndex;
        luts.eotf[static_cast<size_t>(i)] = PqEotf(t);
        float v = Bt709LinearToSdrChannel(t, peak_scale) * 255.0f + 0.5f;
        if (v < 0.0f) {
            v = 0.0f;
        }
        if (v > 255.0f) {
            v = 255.0f;
        }
        luts.sdr[static_cast<size_t>(i)] = static_cast<uint8_t>(v);
    }
    return luts;
}

inline float ClampUnit(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

inline int LutIndex(float unit_value) {
    const int idx = static_cast<int>(unit_value * kLutMaxIndex + 0.5f);
    if (idx < 0) {
        return 0;
    }
    if (idx > kLutSize - 1) {
        return kLutSize - 1;
    }
    return idx;
}

} // namespace

void ConvertP010PqToMonitorBgra(const PlanarYuv420Frame& src, float peak_scale, uint8_t* out_bgra,
                                uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.uv_plane == nullptr || out_bgra == nullptr)
        return;

    const MonitorLuts luts = BuildLuts(peak_scale);

    // Chroma constants for the inverse Y'CbCr (BT.2020 NCL) recombination.
    constexpr float kCrToR = 2.0f * (1.0f - kKr2020);
    constexpr float kCbToB = 2.0f * (1.0f - kKb2020);
    constexpr float kInvKg = 1.0f / kKg2020;

    // P010: 16-bit little-endian words, 10 active bits left-justified in bits
    // 15:6 (low 6 bits zero per the DXGI_FORMAT_P010 definition). 4:2:0 shares one
    // chroma pair across two horizontal pixels, so the chroma-derived terms are
    // computed once per pair; only the luma term varies within the pair.
    for (uint32_t row = 0; row < src.height; ++row) {
        const auto* y_row =
            reinterpret_cast<const uint16_t*>(src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes);
        const auto* uv_row =
            reinterpret_cast<const uint16_t*>(src.uv_plane + static_cast<size_t>(row / 2u) * src.uv_stride_bytes);
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; col += 2u) {
            const float cb = DequantC10Limited(static_cast<uint16_t>(uv_row[col] >> 6));
            const float cr = DequantC10Limited(static_cast<uint16_t>(uv_row[col + 1u] >> 6));
            const float cr_r = cr * kCrToR;
            const float cb_b = cb * kCbToB;
            // g' = (y' - kr*r' - kb*b') / kg, with r'=y'+cr_r, b'=y'+cb_b, so the
            // y'-independent part of g' is a per-pair constant.
            const float g_chroma = -(kKr2020 * cr_r + kKb2020 * cb_b) * kInvKg;
            const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
            for (uint32_t p = col; p < pair_end; ++p) {
                const float yv = DequantY10Limited(static_cast<uint16_t>(y_row[p] >> 6));
                // Inverse Y'CbCr -> PQ-encoded R'G'B'.
                const float rp = yv + cr_r;
                const float bp = yv + cb_b;
                const float gp = yv + g_chroma;
                // PQ EOTF -> BT.2020 normalised linear.
                const LinearRgb lin2020{luts.eotf[static_cast<size_t>(LutIndex(ClampUnit(rp)))],
                                        luts.eotf[static_cast<size_t>(LutIndex(ClampUnit(gp)))],
                                        luts.eotf[static_cast<size_t>(LutIndex(ClampUnit(bp)))]};
                // Gamut to BT.709 linear, then tone-map + OETF via the SDR table.
                const LinearRgb lin709 = Bt2020ToBt709(lin2020);
                uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
                px[0] = luts.sdr[static_cast<size_t>(LutIndex(ClampUnit(lin709.b)))]; // B
                px[1] = luts.sdr[static_cast<size_t>(LutIndex(ClampUnit(lin709.g)))]; // G
                px[2] = luts.sdr[static_cast<size_t>(LutIndex(ClampUnit(lin709.r)))]; // R
                px[3] = 255u;                                                         // A
            }
        }
    }
}

} // namespace recorder_core
