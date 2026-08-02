#include "hdr_preview.h"

#include <cstdint>

namespace recorder_core {

namespace {

constexpr int kLutMaxIndexInt = 1023; // P010PqMonitorConverter::kLutSize - 1
constexpr float kLutMaxIndex = static_cast<float>(kLutMaxIndexInt);

inline float ClampUnit(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

inline size_t LutIndex(float unit_value) {
    const int idx = static_cast<int>(unit_value * kLutMaxIndex + 0.5f);
    if (idx < 0) {
        return 0;
    }
    if (idx > kLutMaxIndexInt) {
        return static_cast<size_t>(kLutMaxIndexInt);
    }
    return static_cast<size_t>(idx);
}

} // namespace

P010PqMonitorConverter::P010PqMonitorConverter(float peak_scale) {
    static_assert(kLutSize == kLutMaxIndexInt + 1);
    for (int i = 0; i < kLutSize; ++i) {
        const float t = static_cast<float>(i) / kLutMaxIndex;
        eotf_lut_[static_cast<size_t>(i)] = PqEotf(t);
        float v = Bt709LinearToSdrChannel(t, peak_scale) * 255.0f + 0.5f;
        if (v < 0.0f) {
            v = 0.0f;
        }
        if (v > 255.0f) {
            v = 255.0f;
        }
        sdr_lut_[static_cast<size_t>(i)] = static_cast<uint8_t>(v);
    }
}

namespace {

// Chroma constants for the inverse Y'CbCr (BT.2020 NCL) recombination.
constexpr float kCrToR = 2.0f * (1.0f - kKr2020);
constexpr float kCbToB = 2.0f * (1.0f - kKb2020);
constexpr float kInvKg = 1.0f / kKg2020;

// The chroma-derived terms of one 4:2:0 chroma pair, shared by the two
// horizontal pixels that sample it. g' = (y' - kr*r' - kb*b') / kg with
// r'=y'+cr_r and b'=y'+cb_b, so the y'-independent part of g' is constant
// across the pair too.
struct ChromaTerms {
    float cr_r;
    float cb_b;
    float g_chroma;
};

inline ChromaTerms ChromaTermsFrom(float cb, float cr) {
    const float cr_r = cr * kCrToR;
    const float cb_b = cb * kCbToB;
    return ChromaTerms{cr_r, cb_b, -(kKr2020 * cr_r + kKb2020 * cb_b) * kInvKg};
}

} // namespace

void P010PqMonitorConverter::WriteMonitorPixel(float yv, float cr_r, float cb_b, float g_chroma, uint8_t* px) const {
    // Inverse Y'CbCr -> PQ-encoded R'G'B'.
    const float rp = yv + cr_r;
    const float bp = yv + cb_b;
    const float gp = yv + g_chroma;
    // PQ EOTF -> BT.2020 normalised linear.
    const LinearRgb lin2020{eotf_lut_[LutIndex(ClampUnit(rp))], eotf_lut_[LutIndex(ClampUnit(gp))],
                            eotf_lut_[LutIndex(ClampUnit(bp))]};
    // Gamut to BT.709 linear, then tone-map + OETF via the SDR table.
    const LinearRgb lin709 = Bt2020ToBt709(lin2020);
    px[0] = sdr_lut_[LutIndex(ClampUnit(lin709.b))]; // B
    px[1] = sdr_lut_[LutIndex(ClampUnit(lin709.g))]; // G
    px[2] = sdr_lut_[LutIndex(ClampUnit(lin709.r))]; // R
    px[3] = 255u;                                    // A
}

void P010PqMonitorConverter::Convert(const PlanarYuv420Frame& src, uint8_t* out_bgra, uint32_t out_stride_bytes) const {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.uv_plane == nullptr || out_bgra == nullptr)
        return;

    // P010: 16-bit little-endian words, 10 active bits left-justified in bits
    // 15:6 (low 6 bits zero per the DXGI_FORMAT_P010 definition).
    for (uint32_t row = 0; row < src.height; ++row) {
        const auto* y_row =
            reinterpret_cast<const uint16_t*>(src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes);
        const auto* uv_row =
            reinterpret_cast<const uint16_t*>(src.uv_plane + static_cast<size_t>(row / 2u) * src.uv_stride_bytes);
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; col += 2u) {
            const ChromaTerms ct = ChromaTermsFrom(DequantC10Limited(static_cast<uint16_t>(uv_row[col] >> 6)),
                                                   DequantC10Limited(static_cast<uint16_t>(uv_row[col + 1u] >> 6)));
            const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
            for (uint32_t p = col; p < pair_end; ++p) {
                const float yv = DequantY10Limited(static_cast<uint16_t>(y_row[p] >> 6));
                WriteMonitorPixel(yv, ct.cr_r, ct.cb_b, ct.g_chroma, out_row + static_cast<size_t>(p) * 4u);
            }
        }
    }
}

void P010PqMonitorConverter::Convert(const FullPlanarYuv420Frame& src, uint8_t* out_bgra,
                                     uint32_t out_stride_bytes) const {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    // YUV420P10LE: plain 16-bit little-endian samples in [0, 1023] (no P010
    // left-justification), U and V in planes of their own at half resolution in
    // both axes -- so one chroma sample serves a 2x2 pixel block and is indexed
    // by col/2, where the P010 layout above walks an interleaved row.
    for (uint32_t row = 0; row < src.height; ++row) {
        const auto* y_row =
            reinterpret_cast<const uint16_t*>(src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes);
        const auto* u_row =
            reinterpret_cast<const uint16_t*>(src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes);
        const auto* v_row =
            reinterpret_cast<const uint16_t*>(src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes);
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; col += 2u) {
            const uint32_t chroma_col = col / 2u;
            const ChromaTerms ct =
                ChromaTermsFrom(DequantC10Limited(u_row[chroma_col]), DequantC10Limited(v_row[chroma_col]));
            const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
            for (uint32_t p = col; p < pair_end; ++p) {
                WriteMonitorPixel(DequantY10Limited(y_row[p]), ct.cr_r, ct.cb_b, ct.g_chroma,
                                  out_row + static_cast<size_t>(p) * 4u);
            }
        }
    }
}

} // namespace recorder_core
