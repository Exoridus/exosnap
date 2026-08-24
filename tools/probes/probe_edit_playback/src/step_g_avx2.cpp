// Step G, variant 4: hand-written AVX2 intrinsics, 16 pixels (8 chroma
// pairs) per iteration. This TU is compiled with /arch:AVX2 (see
// CMakeLists.txt). Only safe to invoke on a CPU where
// probe_g::CpuSupportsAvx2() (step_g_cpuid.cpp, no /arch flag) returned true.
//
// Same 4:2:0 structure as the scalar original: one chroma sample pair feeds
// two horizontal pixels; coefficients come from the same fixed-point
// ComputeCoefs math (see fixed_coefs_copy.h).

#include "fixed_coefs_copy.h"
#include "step_g_simd.h"

#include <immintrin.h>

namespace probe_g {

namespace {

// Reduces 8 int32 lanes (already luma+term sums for an 8-pixel half of the
// 16-pixel batch) to a 128-bit register holding the 8 clamped byte results
// duplicated twice: [v0..v7, v0..v7] -- the same layout the SSE variant's
// ClampAndDup8 produces, so the BGRA interleave below is structurally the
// same as the SSE version, just invoked twice per 16-pixel batch.
//
// _mm256_packus_epi32/epi16 operate independently within each 128-bit half
// of the register ("lane-local"), so a naive pack scrambles element order
// across the 128-bit boundary; _mm256_permute4x64_epi64(..., 0xD8) is the
// standard fix that restores linear order after each pack step.
inline __m128i ClampAndDup8(__m256i sum8) {
    const __m256i round = _mm256_set1_epi32(kFixedRound);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i maxv = _mm256_set1_epi32(255);
    __m256i r = _mm256_srai_epi32(_mm256_add_epi32(sum8, round), kFixedShift);
    r = _mm256_min_epi32(_mm256_max_epi32(r, zero), maxv);

    const __m256i v16 = _mm256_permute4x64_epi64(_mm256_packus_epi32(r, r), 0xD8);
    const __m256i v8 = _mm256_permute4x64_epi64(_mm256_packus_epi16(v16, v16), 0xD8);
    return _mm256_castsi256_si128(v8); // low 128 bits: [v0..v7, v0..v7]
}

inline void StoreBgra8(uint8_t* out, __m128i Bd, __m128i Gd, __m128i Rd, __m128i aByte) {
    const __m128i BG = _mm_unpacklo_epi8(Bd, Gd); // B0G0..B7G7 (16 bytes, all 8 px)
    const __m128i RA = _mm_unpacklo_epi8(Rd, aByte);
    const __m128i bgraLo = _mm_unpacklo_epi16(BG, RA); // first 4 of these 8 pixels
    const __m128i bgraHi = _mm_unpackhi_epi16(BG, RA); // last 4 of these 8 pixels
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), bgraLo);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), bgraHi);
}

} // namespace

void ConvertFullPlanarYuv420ToBgra_AVX2(const exosnap::engine::FullPlanarYuv420Frame& src,
                                         const exosnap::engine::YuvToBgraParams& params, uint8_t* out_bgra,
                                         uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    const __m128i aByte = _mm_set1_epi8(static_cast<char>(255));
    const __m256i cOff = _mm256_set1_epi32(c.c_off);
    const __m256i yOff = _mm256_set1_epi32(c.y_off);
    const __m256i cY = _mm256_set1_epi32(c.c_y);
    const __m256i cBu = _mm256_set1_epi32(c.c_bu);
    const __m256i cGu = _mm256_set1_epi32(c.c_gu);
    const __m256i cGv = _mm256_set1_epi32(c.c_gv);
    const __m256i cRv = _mm256_set1_epi32(c.c_rv);

    // Permute-index vectors that duplicate each of 8 pair-terms into the
    // 8-pixel-group it feeds: pairs [p0..p7] ->
    //   idxLo -> pixels 0-7:  [p0,p0,p1,p1,p2,p2,p3,p3]
    //   idxHi -> pixels 8-15: [p4,p4,p5,p5,p6,p6,p7,p7]
    const __m256i idxLo = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    const __m256i idxHi = _mm256_setr_epi32(4, 4, 5, 5, 6, 6, 7, 7);

    const uint32_t widthAligned = src.width - (src.width % 16u);

    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;

        uint32_t col = 0;
        for (; col < widthAligned; col += 16u) {
            // 16 pixels = 8 chroma pairs.
            const __m128i yBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y_row + col));    // 16 bytes
            const __m128i uBytes8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(u_row + col / 2u)); // 8 bytes
            const __m128i vBytes8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v_row + col / 2u)); // 8 bytes

            const __m256i yLo = _mm256_cvtepu8_epi32(yBytes);                          // pixels 0-7
            const __m256i yHi = _mm256_cvtepu8_epi32(_mm_srli_si128(yBytes, 8));       // pixels 8-15
            const __m256i u8 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(uBytes8), cOff);  // pairs 0-7
            const __m256i v8 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(vBytes8), cOff);  // pairs 0-7

            const __m256i bTermPair = _mm256_mullo_epi32(cBu, u8);
            const __m256i gTermPair = _mm256_sub_epi32(
                _mm256_setzero_si256(), _mm256_add_epi32(_mm256_mullo_epi32(cGu, u8), _mm256_mullo_epi32(cGv, v8)));
            const __m256i rTermPair = _mm256_mullo_epi32(cRv, v8);

            const __m256i bTermLo = _mm256_permutevar8x32_epi32(bTermPair, idxLo);
            const __m256i bTermHi = _mm256_permutevar8x32_epi32(bTermPair, idxHi);
            const __m256i gTermLo = _mm256_permutevar8x32_epi32(gTermPair, idxLo);
            const __m256i gTermHi = _mm256_permutevar8x32_epi32(gTermPair, idxHi);
            const __m256i rTermLo = _mm256_permutevar8x32_epi32(rTermPair, idxLo);
            const __m256i rTermHi = _mm256_permutevar8x32_epi32(rTermPair, idxHi);

            const __m256i lumaLo = _mm256_mullo_epi32(cY, _mm256_sub_epi32(yLo, yOff));
            const __m256i lumaHi = _mm256_mullo_epi32(cY, _mm256_sub_epi32(yHi, yOff));

            const __m128i BdLo = ClampAndDup8(_mm256_add_epi32(lumaLo, bTermLo));
            const __m128i BdHi = ClampAndDup8(_mm256_add_epi32(lumaHi, bTermHi));
            const __m128i GdLo = ClampAndDup8(_mm256_add_epi32(lumaLo, gTermLo));
            const __m128i GdHi = ClampAndDup8(_mm256_add_epi32(lumaHi, gTermHi));
            const __m128i RdLo = ClampAndDup8(_mm256_add_epi32(lumaLo, rTermLo));
            const __m128i RdHi = ClampAndDup8(_mm256_add_epi32(lumaHi, rTermHi));

            uint8_t* out = out_row + static_cast<size_t>(col) * 4u;
            StoreBgra8(out, BdLo, GdLo, RdLo, aByte);      // pixels 0-7
            StoreBgra8(out + 32, BdHi, GdHi, RdHi, aByte); // pixels 8-15
        }

        // Scalar tail for any width not a multiple of 16 (not exercised by
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
