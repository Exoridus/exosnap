// Step G, variant 3: hand-written SSE2/SSE4.1 intrinsics, 8 pixels (4 chroma
// pairs) per iteration. SSE2 is guaranteed present on every x64 CPU, so this
// is the number that matters for the weakest/oldest hardware ExoSnap can
// run on (SSE4.1 -- 2008-era Penryn onward -- is used only for
// _mm_mullo_epi32/_mm_packus_epi32, both effectively universal today).
//
// Follows the same 4:2:0 structure as the scalar original
// (exosnap::engine::ConvertFullPlanarYuv420ToBgra, yuv_to_bgra.cpp): one
// chroma sample pair feeds two horizontal pixels; coefficients come from the
// same fixed-point ComputeCoefs math (see fixed_coefs_copy.h).

#include "fixed_coefs_copy.h"
#include "step_g_simd.h"

#include <immintrin.h>

#include <cstring>

namespace probe_g {

namespace {

// Rounds, shifts by kFixedShift, clamps to [0,255], and narrows 4+4 int32
// lanes (already luma+term sums for 8 pixels) down to a single 128-bit
// register holding the 8 byte results duplicated twice: [v0..v7, v0..v7].
// That duplicated layout is exactly what the unpacklo_epi8-based BGRA
// interleave below needs (see the AVX2 variant's ClampAndDup8 for the same
// idiom scaled to 256-bit).
inline __m128i ClampAndDup8(__m128i lo4, __m128i hi4) {
    const __m128i round = _mm_set1_epi32(kFixedRound);
    const __m128i zero = _mm_setzero_si128();
    const __m128i maxv = _mm_set1_epi32(255);
    __m128i rl = _mm_srai_epi32(_mm_add_epi32(lo4, round), kFixedShift);
    __m128i rh = _mm_srai_epi32(_mm_add_epi32(hi4, round), kFixedShift);
    rl = _mm_min_epi32(_mm_max_epi32(rl, zero), maxv);
    rh = _mm_min_epi32(_mm_max_epi32(rh, zero), maxv);
    const __m128i packed16 = _mm_packus_epi32(rl, rh); // 8x uint16, values 0..255
    return _mm_packus_epi16(packed16, packed16);       // 16 bytes: [v0..v7, v0..v7]
}

} // namespace

void ConvertFullPlanarYuv420ToBgra_SSE(const exosnap::engine::FullPlanarYuv420Frame& src,
                                        const exosnap::engine::YuvToBgraParams& params, uint8_t* out_bgra,
                                        uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    const __m128i aByte = _mm_set1_epi8(static_cast<char>(255));
    const __m128i cOff = _mm_set1_epi32(c.c_off);
    const __m128i yOff = _mm_set1_epi32(c.y_off);
    const __m128i cY = _mm_set1_epi32(c.c_y);
    const __m128i cBu = _mm_set1_epi32(c.c_bu);
    const __m128i cGu = _mm_set1_epi32(c.c_gu);
    const __m128i cGv = _mm_set1_epi32(c.c_gv);
    const __m128i cRv = _mm_set1_epi32(c.c_rv);

    const uint32_t widthAligned = src.width - (src.width % 8u);

    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;

        uint32_t col = 0;
        for (; col < widthAligned; col += 8u) {
            // 8 pixels = 4 chroma pairs.
            const __m128i yBytes = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(y_row + col)); // 8 bytes

            int32_t uTmp = 0;
            int32_t vTmp = 0;
            std::memcpy(&uTmp, u_row + col / 2u, sizeof(uTmp)); // 4 bytes, 4 chroma pairs
            std::memcpy(&vTmp, v_row + col / 2u, sizeof(vTmp));
            const __m128i uBytes = _mm_cvtsi32_si128(uTmp);
            const __m128i vBytes = _mm_cvtsi32_si128(vTmp);

            const __m128i yLo = _mm_cvtepu8_epi32(yBytes);                    // pixels 0-3
            const __m128i yHi = _mm_cvtepu8_epi32(_mm_srli_si128(yBytes, 4)); // pixels 4-7
            const __m128i u32 = _mm_sub_epi32(_mm_cvtepu8_epi32(uBytes), cOff); // pairs 0-3
            const __m128i v32 = _mm_sub_epi32(_mm_cvtepu8_epi32(vBytes), cOff); // pairs 0-3

            const __m128i bTermPair = _mm_mullo_epi32(cBu, u32);
            const __m128i gTermPair = _mm_sub_epi32(
                _mm_setzero_si128(), _mm_add_epi32(_mm_mullo_epi32(cGu, u32), _mm_mullo_epi32(cGv, v32)));
            const __m128i rTermPair = _mm_mullo_epi32(cRv, v32);

            // Duplicate each of the 4 pair-terms across its 2 pixels:
            // [t0,t1,t2,t3] -> lo=[t0,t0,t1,t1], hi=[t2,t2,t3,t3].
            const __m128i bTermLo = _mm_unpacklo_epi32(bTermPair, bTermPair);
            const __m128i bTermHi = _mm_unpackhi_epi32(bTermPair, bTermPair);
            const __m128i gTermLo = _mm_unpacklo_epi32(gTermPair, gTermPair);
            const __m128i gTermHi = _mm_unpackhi_epi32(gTermPair, gTermPair);
            const __m128i rTermLo = _mm_unpacklo_epi32(rTermPair, rTermPair);
            const __m128i rTermHi = _mm_unpackhi_epi32(rTermPair, rTermPair);

            const __m128i lumaLo = _mm_mullo_epi32(cY, _mm_sub_epi32(yLo, yOff));
            const __m128i lumaHi = _mm_mullo_epi32(cY, _mm_sub_epi32(yHi, yOff));

            const __m128i Bd = ClampAndDup8(_mm_add_epi32(lumaLo, bTermLo), _mm_add_epi32(lumaHi, bTermHi));
            const __m128i Gd = ClampAndDup8(_mm_add_epi32(lumaLo, gTermLo), _mm_add_epi32(lumaHi, gTermHi));
            const __m128i Rd = ClampAndDup8(_mm_add_epi32(lumaLo, rTermLo), _mm_add_epi32(lumaHi, rTermHi));

            const __m128i BG = _mm_unpacklo_epi8(Bd, Gd); // B0G0B1G1...B7G7 (16 bytes, all 8 px)
            const __m128i RA = _mm_unpacklo_epi8(Rd, aByte);

            const __m128i bgraLo = _mm_unpacklo_epi16(BG, RA); // pixels 0-3
            const __m128i bgraHi = _mm_unpackhi_epi16(BG, RA); // pixels 4-7

            uint8_t* out = out_row + static_cast<size_t>(col) * 4u;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out), bgraLo);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), bgraHi);
        }

        // Scalar tail for any width not a multiple of 8 (not exercised by
        // the probe's 2560-wide dummy frame, kept for correctness/generality).
        for (; col < src.width; col += 2u) {
            const int32_t u_val = static_cast<int32_t>(u_row[col / 2u]) - c.c_off;
            const int32_t v_val = static_cast<int32_t>(v_row[col / 2u]) - c.c_off;
            const int32_t b_term = c.c_bu * u_val;
            const int32_t g_term = -c.c_gu * u_val - c.c_gv * v_val;
            const int32_t r_term = c.c_rv * v_val;
            const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
            for (uint32_t p = col; p < pair_end; ++p) {
                const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[p]) - c.y_off);
                uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
                px[0] = ClampFixedToByte(luma + b_term);
                px[1] = ClampFixedToByte(luma + g_term);
                px[2] = ClampFixedToByte(luma + r_term);
                px[3] = 255u;
            }
        }
    }
}

} // namespace probe_g
