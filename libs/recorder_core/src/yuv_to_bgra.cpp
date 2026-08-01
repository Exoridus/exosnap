#include "yuv_to_bgra.h"

// The SIMD path below is x86-only. Everything stays buildable elsewhere: the
// capability query then reports false and the dispatcher never leaves the
// scalar reference implementation.
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define EXOSNAP_YUV_TO_BGRA_HAS_SIMD 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#include <cstring>
#else
#define EXOSNAP_YUV_TO_BGRA_HAS_SIMD 0
#endif

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

// One 4:2:0 chroma pair -- i.e. up to two horizontal pixels sharing one U/V
// sample -- of an 8-bit fully-planar frame. Factored out so the SIMD path's
// tail (for widths that are not a multiple of its vector width) runs literally
// the same arithmetic as the scalar reference rather than a copy of it.
inline void ConvertFullPlanarPair8(const uint8_t* y_row, const uint8_t* u_row, const uint8_t* v_row, uint8_t* out_row,
                                   uint32_t col, uint32_t width, const FixedCoefs& c) noexcept {
    const int32_t u_val = static_cast<int32_t>(u_row[col / 2u]) - c.c_off;
    const int32_t v_val = static_cast<int32_t>(v_row[col / 2u]) - c.c_off;
    const int32_t b_term = c.c_bu * u_val;
    const int32_t g_term = -c.c_gu * u_val - c.c_gv * v_val;
    const int32_t r_term = c.c_rv * v_val;
    const uint32_t pair_end = (col + 2u <= width) ? (col + 2u) : width;
    for (uint32_t p = col; p < pair_end; ++p) {
        const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[p]) - c.y_off);
        uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
        px[0] = ClampFixedToByte(luma + b_term); // B
        px[1] = ClampFixedToByte(luma + g_term); // G
        px[2] = ClampFixedToByte(luma + r_term); // R
        px[3] = 255u;                            // A
    }
}

// One 4:4:4 pixel of an 8-bit fully-planar frame -- every pixel carries its
// own U and V, so unlike ConvertFullPlanarPair8 above there is no pair/block
// handling at all. Factored out for the same reason: the SIMD path's tail
// (widths not a multiple of its vector width) runs literally the same
// arithmetic as the scalar reference.
inline void ConvertFullPlanar444Pixel8(const uint8_t* y_row, const uint8_t* u_row, const uint8_t* v_row,
                                       uint8_t* out_row, uint32_t col, const FixedCoefs& c) noexcept {
    const int32_t u_val = static_cast<int32_t>(u_row[col]) - c.c_off;
    const int32_t v_val = static_cast<int32_t>(v_row[col]) - c.c_off;
    const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[col]) - c.y_off);
    uint8_t* px = out_row + static_cast<size_t>(col) * 4u;
    px[0] = ClampFixedToByte(luma + c.c_bu * u_val);                  // B
    px[1] = ClampFixedToByte(luma - c.c_gu * u_val - c.c_gv * v_val); // G
    px[2] = ClampFixedToByte(luma + c.c_rv * v_val);                  // R
    px[3] = 255u;                                                     // A
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

void ConvertFullPlanarYuv420ToBgraScalar(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                         uint8_t* out_bgra, uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    if (src.bits_per_sample > 8) {
        // YUV420P10LE: plain 16-bit little-endian values in [0, 1023] (no P010
        // left-justification -- unlike the semi-planar path above).
        for (uint32_t row = 0; row < src.height; ++row) {
            const auto* y_row =
                reinterpret_cast<const uint16_t*>(src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes);
            const auto* u_row =
                reinterpret_cast<const uint16_t*>(src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes);
            const auto* v_row =
                reinterpret_cast<const uint16_t*>(src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes);
            uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
            for (uint32_t col = 0; col < src.width; col += 2u) {
                const int32_t u_val = static_cast<int32_t>(u_row[col / 2u]) - c.c_off;
                const int32_t v_val = static_cast<int32_t>(v_row[col / 2u]) - c.c_off;
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
        return;
    }

    // YUV420P: 8-bit samples, separate U/V planes.
    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; col += 2u)
            ConvertFullPlanarPair8(y_row, u_row, v_row, out_row, col, src.width, c);
    }
}

bool CpuSupportsYuvToBgraSimd() noexcept {
#if EXOSNAP_YUV_TO_BGRA_HAS_SIMD
    // Queried once: CPUID is not free, and the answer cannot change while the
    // process runs. SSE4.1 is CPUID leaf 1, ECX bit 19.
    static const bool supported = []() noexcept {
        int regs[4] = {0, 0, 0, 0};
#if defined(_MSC_VER)
        __cpuid(regs, 1);
#else
        __get_cpuid(1, reinterpret_cast<unsigned*>(&regs[0]), reinterpret_cast<unsigned*>(&regs[1]),
                    reinterpret_cast<unsigned*>(&regs[2]), reinterpret_cast<unsigned*>(&regs[3]));
#endif
        return (regs[2] & (1 << 19)) != 0;
    }();
    return supported;
#else
    return false;
#endif
}

#if EXOSNAP_YUV_TO_BGRA_HAS_SIMD

namespace {

// Rounds, shifts, clamps to [0,255] and narrows two vectors of four int32
// lanes into one register holding the eight resulting bytes twice over --
// the layout the unpacklo-based BGRA interleave below consumes.
inline __m128i ClampFixedToBytesDuplicated(__m128i lo4, __m128i hi4) noexcept {
    const __m128i round = _mm_set1_epi32(kFixedRound);
    const __m128i zero = _mm_setzero_si128();
    const __m128i maxv = _mm_set1_epi32(255);
    __m128i rl = _mm_srai_epi32(_mm_add_epi32(lo4, round), kFixedShift);
    __m128i rh = _mm_srai_epi32(_mm_add_epi32(hi4, round), kFixedShift);
    rl = _mm_min_epi32(_mm_max_epi32(rl, zero), maxv);
    rh = _mm_min_epi32(_mm_max_epi32(rh, zero), maxv);
    const __m128i packed16 = _mm_packus_epi32(rl, rh);
    return _mm_packus_epi16(packed16, packed16);
}

} // namespace

void ConvertFullPlanarYuv420ToBgraSimd(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                       uint8_t* out_bgra, uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;
    if (src.bits_per_sample > 8) {
        // 10-bit is not the hot path (no measurement justifies a second
        // vectorised kernel yet) -- hand it to the reference implementation.
        ConvertFullPlanarYuv420ToBgraScalar(src, params, out_bgra, out_stride_bytes);
        return;
    }

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    const __m128i alpha = _mm_set1_epi8(static_cast<char>(255));
    const __m128i c_off = _mm_set1_epi32(c.c_off);
    const __m128i y_off = _mm_set1_epi32(c.y_off);
    const __m128i c_y = _mm_set1_epi32(c.c_y);
    const __m128i c_bu = _mm_set1_epi32(c.c_bu);
    const __m128i c_gu = _mm_set1_epi32(c.c_gu);
    const __m128i c_gv = _mm_set1_epi32(c.c_gv);
    const __m128i c_rv = _mm_set1_epi32(c.c_rv);

    // Whole 8-pixel blocks only; the remainder is finished pair-by-pair below
    // with the exact same arithmetic the scalar reference uses.
    const uint32_t width_aligned = src.width - (src.width % 8u);

    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;

        uint32_t col = 0;
        for (; col < width_aligned; col += 8u) {
            // 8 luma samples, and the 4 chroma pairs they share.
            const __m128i y_bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(y_row + col));
            int32_t u_tmp = 0;
            int32_t v_tmp = 0;
            std::memcpy(&u_tmp, u_row + col / 2u, sizeof(u_tmp));
            std::memcpy(&v_tmp, v_row + col / 2u, sizeof(v_tmp));

            const __m128i y_lo = _mm_cvtepu8_epi32(y_bytes);                    // pixels 0-3
            const __m128i y_hi = _mm_cvtepu8_epi32(_mm_srli_si128(y_bytes, 4)); // pixels 4-7
            const __m128i u32 = _mm_sub_epi32(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(u_tmp)), c_off);
            const __m128i v32 = _mm_sub_epi32(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(v_tmp)), c_off);

            const __m128i b_pair = _mm_mullo_epi32(c_bu, u32);
            const __m128i g_pair = _mm_sub_epi32(_mm_setzero_si128(),
                                                 _mm_add_epi32(_mm_mullo_epi32(c_gu, u32), _mm_mullo_epi32(c_gv, v32)));
            const __m128i r_pair = _mm_mullo_epi32(c_rv, v32);

            // Each chroma term serves two horizontal pixels (4:2:0), so widen
            // [t0,t1,t2,t3] to [t0,t0,t1,t1] and [t2,t2,t3,t3].
            const __m128i luma_lo = _mm_mullo_epi32(c_y, _mm_sub_epi32(y_lo, y_off));
            const __m128i luma_hi = _mm_mullo_epi32(c_y, _mm_sub_epi32(y_hi, y_off));

            const __m128i b = ClampFixedToBytesDuplicated(_mm_add_epi32(luma_lo, _mm_unpacklo_epi32(b_pair, b_pair)),
                                                          _mm_add_epi32(luma_hi, _mm_unpackhi_epi32(b_pair, b_pair)));
            const __m128i g = ClampFixedToBytesDuplicated(_mm_add_epi32(luma_lo, _mm_unpacklo_epi32(g_pair, g_pair)),
                                                          _mm_add_epi32(luma_hi, _mm_unpackhi_epi32(g_pair, g_pair)));
            const __m128i r = ClampFixedToBytesDuplicated(_mm_add_epi32(luma_lo, _mm_unpacklo_epi32(r_pair, r_pair)),
                                                          _mm_add_epi32(luma_hi, _mm_unpackhi_epi32(r_pair, r_pair)));

            const __m128i bg = _mm_unpacklo_epi8(b, g);
            const __m128i ra = _mm_unpacklo_epi8(r, alpha);

            uint8_t* out = out_row + static_cast<size_t>(col) * 4u;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_unpacklo_epi16(bg, ra));      // pixels 0-3
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), _mm_unpackhi_epi16(bg, ra)); // pixels 4-7
        }

        for (; col < src.width; col += 2u)
            ConvertFullPlanarPair8(y_row, u_row, v_row, out_row, col, src.width, c);
    }
}

#else // !EXOSNAP_YUV_TO_BGRA_HAS_SIMD

void ConvertFullPlanarYuv420ToBgraSimd(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                       uint8_t* out_bgra, uint32_t out_stride_bytes) {
    ConvertFullPlanarYuv420ToBgraScalar(src, params, out_bgra, out_stride_bytes);
}

#endif

void ConvertFullPlanarYuv420ToBgra(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                   uint32_t out_stride_bytes) {
    if (src.bits_per_sample <= 8 && CpuSupportsYuvToBgraSimd()) {
        ConvertFullPlanarYuv420ToBgraSimd(src, params, out_bgra, out_stride_bytes);
        return;
    }
    ConvertFullPlanarYuv420ToBgraScalar(src, params, out_bgra, out_stride_bytes);
}

void ConvertFullPlanar444ToBgraScalar(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                      uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    // Always 8-bit -- see FullPlanar444Frame's comment for why no 10-bit
    // branch is needed here.
    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, /*bits_per_sample=*/8);

    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; ++col)
            ConvertFullPlanar444Pixel8(y_row, u_row, v_row, out_row, col, c);
    }
}

#if EXOSNAP_YUV_TO_BGRA_HAS_SIMD

void ConvertFullPlanar444ToBgraSimd(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                    uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, /*bits_per_sample=*/8);

    const __m128i alpha = _mm_set1_epi8(static_cast<char>(255));
    const __m128i c_off = _mm_set1_epi32(c.c_off);
    const __m128i y_off = _mm_set1_epi32(c.y_off);
    const __m128i c_y = _mm_set1_epi32(c.c_y);
    const __m128i c_bu = _mm_set1_epi32(c.c_bu);
    const __m128i c_gu = _mm_set1_epi32(c.c_gu);
    const __m128i c_gv = _mm_set1_epi32(c.c_gv);
    const __m128i c_rv = _mm_set1_epi32(c.c_rv);

    // Whole 8-pixel blocks only; the remainder is finished pixel-by-pixel
    // below with the exact same arithmetic the scalar reference uses.
    const uint32_t width_aligned = src.width - (src.width % 8u);

    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;

        uint32_t col = 0;
        for (; col < width_aligned; col += 8u) {
            // 8 luma samples, and each one's own U/V (no chroma widening --
            // that is the whole simplification vs. the 4:2:0 SIMD kernel).
            const __m128i y_bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(y_row + col));
            const __m128i u_bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(u_row + col));
            const __m128i v_bytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v_row + col));

            const __m128i y_lo = _mm_cvtepu8_epi32(y_bytes);                    // pixels 0-3
            const __m128i y_hi = _mm_cvtepu8_epi32(_mm_srli_si128(y_bytes, 4)); // pixels 4-7
            const __m128i u_lo = _mm_sub_epi32(_mm_cvtepu8_epi32(u_bytes), c_off);
            const __m128i u_hi = _mm_sub_epi32(_mm_cvtepu8_epi32(_mm_srli_si128(u_bytes, 4)), c_off);
            const __m128i v_lo = _mm_sub_epi32(_mm_cvtepu8_epi32(v_bytes), c_off);
            const __m128i v_hi = _mm_sub_epi32(_mm_cvtepu8_epi32(_mm_srli_si128(v_bytes, 4)), c_off);

            const __m128i luma_lo = _mm_mullo_epi32(c_y, _mm_sub_epi32(y_lo, y_off));
            const __m128i luma_hi = _mm_mullo_epi32(c_y, _mm_sub_epi32(y_hi, y_off));

            const __m128i b_lo = _mm_mullo_epi32(c_bu, u_lo);
            const __m128i b_hi = _mm_mullo_epi32(c_bu, u_hi);
            const __m128i g_lo = _mm_sub_epi32(_mm_setzero_si128(),
                                               _mm_add_epi32(_mm_mullo_epi32(c_gu, u_lo), _mm_mullo_epi32(c_gv, v_lo)));
            const __m128i g_hi = _mm_sub_epi32(_mm_setzero_si128(),
                                               _mm_add_epi32(_mm_mullo_epi32(c_gu, u_hi), _mm_mullo_epi32(c_gv, v_hi)));
            const __m128i r_lo = _mm_mullo_epi32(c_rv, v_lo);
            const __m128i r_hi = _mm_mullo_epi32(c_rv, v_hi);

            const __m128i b = ClampFixedToBytesDuplicated(_mm_add_epi32(luma_lo, b_lo), _mm_add_epi32(luma_hi, b_hi));
            const __m128i g = ClampFixedToBytesDuplicated(_mm_add_epi32(luma_lo, g_lo), _mm_add_epi32(luma_hi, g_hi));
            const __m128i r = ClampFixedToBytesDuplicated(_mm_add_epi32(luma_lo, r_lo), _mm_add_epi32(luma_hi, r_hi));

            const __m128i bg = _mm_unpacklo_epi8(b, g);
            const __m128i ra = _mm_unpacklo_epi8(r, alpha);

            uint8_t* out = out_row + static_cast<size_t>(col) * 4u;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_unpacklo_epi16(bg, ra));      // pixels 0-3
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), _mm_unpackhi_epi16(bg, ra)); // pixels 4-7
        }

        for (; col < src.width; ++col)
            ConvertFullPlanar444Pixel8(y_row, u_row, v_row, out_row, col, c);
    }
}

#else // !EXOSNAP_YUV_TO_BGRA_HAS_SIMD

void ConvertFullPlanar444ToBgraSimd(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                    uint32_t out_stride_bytes) {
    ConvertFullPlanar444ToBgraScalar(src, params, out_bgra, out_stride_bytes);
}

#endif

void ConvertFullPlanar444ToBgra(const FullPlanar444Frame& src, const YuvToBgraParams& params, uint8_t* out_bgra,
                                uint32_t out_stride_bytes) {
    if (CpuSupportsYuvToBgraSimd()) {
        ConvertFullPlanar444ToBgraSimd(src, params, out_bgra, out_stride_bytes);
        return;
    }
    ConvertFullPlanar444ToBgraScalar(src, params, out_bgra, out_stride_bytes);
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
