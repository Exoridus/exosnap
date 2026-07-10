#include "yuv_to_bgra.h"

namespace recorder_core {

namespace {

struct MatrixWeights {
    double kr;
    double kb;
};

// Rec. ITU-T H.273 / ISO-IEC 23001-8 luma weights for the matrices this
// engine can encounter. Unspecified falls back to BT.709 because that is the
// only matrix this engine's encoder path ever actually produces (see
// ColorMetadata::Sdr709) -- guessing a legacy SD matrix for "unspecified"
// would be worse than assuming the engine's own default.
MatrixWeights WeightsFor(MatrixCoefficients matrix) noexcept {
    switch (matrix) {
    case MatrixCoefficients::Bt601:
        return {0.299, 0.114};
    case MatrixCoefficients::Bt2020Ncl:
        return {0.2627, 0.0593};
    case MatrixCoefficients::Bt709:
    case MatrixCoefficients::Unspecified:
    default:
        return {0.2126, 0.0722};
    }
}

// Integer 16.16 fixed-point conversion coefficients, precomputed once per
// frame. The per-pixel loop is pure integer math (the conversion runs
// synchronously on VideoThread at the preview cadence, so per-pixel doubles
// were measurably too slow at 4K).
//
//   R = (c_y*(Y - y_off)             + c_rv*(V - c_off) + round) >> 16
//   G = (c_y*(Y - y_off) - c_gu*(U - c_off) - c_gv*(V - c_off) + round) >> 16
//   B = (c_y*(Y - y_off) + c_bu*(U - c_off)             + round) >> 16
//
// Value-range check (worst case 10-bit): coefficients < 40'000, sample
// deltas < 1'024 -> per-term products < 2^26 and 3-term sums < 2^28, well
// inside int32_t.
struct FixedCoefs {
    int32_t c_y;
    int32_t c_rv;
    int32_t c_gu;
    int32_t c_gv;
    int32_t c_bu;
    int32_t y_off;
    int32_t c_off;
};

constexpr int kFixedShift = 16;
constexpr int32_t kFixedRound = 1 << (kFixedShift - 1);

FixedCoefs ComputeCoefs(MatrixCoefficients matrix, ColorRange range, uint32_t bits_per_sample) noexcept {
    const MatrixWeights w = WeightsFor(matrix);
    const double kg = 1.0 - w.kr - w.kb;
    const double rv = 2.0 * (1.0 - w.kr);
    const double bu = 2.0 * (1.0 - w.kb);
    const double gu = bu * w.kb / kg;
    const double gv = rv * w.kr / kg;

    const bool ten_bit = bits_per_sample > 8;
    const bool limited = range == ColorRange::Limited;

    // Normalization scales: map (sample - offset) to Y' in [0,1] / C' in
    // [-0.5, 0.5], then to the 0..255 output domain. Folded into one scale.
    // 8-bit:  limited Y (16..235)/219, C (centered 128)/224
    //         full    Y (0..255)/255, C (centered 128)/255
    // 10-bit: limited Y (64..940)/876, C (centered 512)/896
    //         full    Y (0..1023)/1023, C (centered 512)/1023
    double y_scale;
    double c_scale;
    int32_t y_off;
    int32_t c_off;
    if (ten_bit) {
        y_scale = limited ? (255.0 / 876.0) : (255.0 / 1023.0);
        c_scale = limited ? (255.0 / 896.0) : (255.0 / 1023.0);
        y_off = limited ? 64 : 0;
        c_off = 512;
    } else {
        y_scale = limited ? (255.0 / 219.0) : 1.0;
        c_scale = limited ? (255.0 / 224.0) : 1.0;
        y_off = limited ? 16 : 0;
        c_off = 128;
    }

    const double fixed_one = static_cast<double>(1 << kFixedShift);
    auto to_fixed = [fixed_one](double v) noexcept { return static_cast<int32_t>(v * fixed_one + 0.5); };

    FixedCoefs c{};
    c.c_y = to_fixed(y_scale);
    c.c_rv = to_fixed(rv * c_scale);
    c.c_gu = to_fixed(gu * c_scale);
    c.c_gv = to_fixed(gv * c_scale);
    c.c_bu = to_fixed(bu * c_scale);
    c.y_off = y_off;
    c.c_off = c_off;
    return c;
}

inline uint8_t ClampFixedToByte(int32_t fixed) noexcept {
    const int32_t v = (fixed + kFixedRound) >> kFixedShift;
    if (v <= 0)
        return 0;
    if (v >= 255)
        return 255;
    return static_cast<uint8_t>(v);
}

} // namespace

void ConvertYuv420ToBgra(const PlanarYuv420Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                         uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.uv_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    // 4:2:0 lets each chroma pair serve two horizontal pixels, so the three
    // chroma products are computed once per PAIR (measurably faster than
    // per-pixel chroma math at 4K; only the luma term varies within a pair).

    if (src.bits_per_sample > 8) {
        // P010: 16-bit little-endian words, 10 active bits left-justified in
        // bits 15:6 (low 6 bits are 0 per the DXGI_FORMAT_P010 definition).
        for (uint32_t row = 0; row < src.height; ++row) {
            const auto* y_row =
                reinterpret_cast<const uint16_t*>(src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes);
            const auto* uv_row =
                reinterpret_cast<const uint16_t*>(src.uv_plane + static_cast<size_t>(row / 2u) * src.uv_stride_bytes);
            uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
            for (uint32_t col = 0; col < src.width; col += 2u) {
                const int32_t u_val = static_cast<int32_t>(uv_row[col] >> 6) - c.c_off;
                const int32_t v_val = static_cast<int32_t>(uv_row[col + 1u] >> 6) - c.c_off;
                const int32_t b_term = c.c_bu * u_val;
                const int32_t g_term = -c.c_gu * u_val - c.c_gv * v_val;
                const int32_t r_term = c.c_rv * v_val;
                const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
                for (uint32_t p = col; p < pair_end; ++p) {
                    const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[p] >> 6) - c.y_off);
                    uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
                    px[0] = ClampFixedToByte(luma + b_term); // B
                    px[1] = ClampFixedToByte(luma + g_term); // G
                    px[2] = ClampFixedToByte(luma + r_term); // R
                    px[3] = 255u;                            // A
                }
            }
        }
        return;
    }

    // NV12: 8-bit samples.
    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* uv_row = src.uv_plane + static_cast<size_t>(row / 2u) * src.uv_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; col += 2u) {
            const int32_t u_val = static_cast<int32_t>(uv_row[col]) - c.c_off;
            const int32_t v_val = static_cast<int32_t>(uv_row[col + 1u]) - c.c_off;
            const int32_t b_term = c.c_bu * u_val;
            const int32_t g_term = -c.c_gu * u_val - c.c_gv * v_val;
            const int32_t r_term = c.c_rv * v_val;
            const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
            for (uint32_t p = col; p < pair_end; ++p) {
                const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[p]) - c.y_off);
                uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
                px[0] = ClampFixedToByte(luma + b_term); // B
                px[1] = ClampFixedToByte(luma + g_term); // G
                px[2] = ClampFixedToByte(luma + r_term); // R
                px[3] = 255u;                            // A
            }
        }
    }
}

void ConvertAyuvToBgra(const PackedAyuvFrame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                       uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.data == nullptr || out_bgra == nullptr)
        return;

    // Packed 4:4:4 AYUV is always 8-bit single-plane; reuse the shared 8-bit
    // fixed-point coefficients so this decode is the exact inverse of the
    // RGB->AYUV encoder shader (same BT.709 matrix + Full/Limited range math as
    // the NV12 branch). No chroma subsampling: each pixel carries its own V,U,Y.
    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, /*bits_per_sample=*/8);

    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* in_row = src.data + static_cast<size_t>(row) * src.stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; ++col) {
            const uint8_t* p = in_row + static_cast<size_t>(col) * 4u; // [V, U, Y, A]
            const int32_t v_val = static_cast<int32_t>(p[0]) - c.c_off;
            const int32_t u_val = static_cast<int32_t>(p[1]) - c.c_off;
            const int32_t luma = c.c_y * (static_cast<int32_t>(p[2]) - c.y_off);
            uint8_t* px = out_row + static_cast<size_t>(col) * 4u;
            px[0] = ClampFixedToByte(luma + c.c_bu * u_val);                  // B
            px[1] = ClampFixedToByte(luma - c.c_gu * u_val - c.c_gv * v_val); // G
            px[2] = ClampFixedToByte(luma + c.c_rv * v_val);                  // R
            px[3] = 255u;                                                     // A
        }
    }
}

} // namespace recorder_core
