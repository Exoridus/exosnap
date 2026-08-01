// Step G, variant 2 (auto-vectorization): this TU is compiled with
// /arch:AVX2 (see CMakeLists.txt) and nothing else -- the loop body below is
// an unmodified, 1:1 copy of the 8-bit branch of
// recorder_core::ConvertFullPlanarYuv420ToBgra (libs/recorder_core/src/yuv_to_bgra.cpp).
// The only variable is the compiler's target ISA; this answers the cheapest
// possible question first: does /arch:AVX2 alone let the MSVC
// auto-vectorizer turn this into SIMD code, with zero source changes?

#include "fixed_coefs_copy.h"
#include "step_g_simd.h"

namespace probe_g {

void ConvertFullPlanarYuv420ToBgra_AutoVec(const recorder_core::FullPlanarYuv420Frame& src,
                                            const recorder_core::YuvToBgraParams& params, uint8_t* out_bgra,
                                            uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    // YUV420P: 8-bit samples, separate U/V planes. Verbatim copy of the
    // scalar loop body in yuv_to_bgra.cpp's ConvertFullPlanarYuv420ToBgra.
    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes;
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
}

} // namespace probe_g
