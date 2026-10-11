#pragma once

#include <exosnap/engine/color_metadata.h>

namespace exosnap::engine {

// Constant buffer shared by the GPU YUV -> RGB shaders (editor playback and
// webcam upload). 32 bytes: constant buffers are 16-byte aligned. The shader
// computes, from raw integer codes:
//   yn = (y - y_off) * y_scale;  un = u - c_off;  vn = v - c_off;
//   r = yn + c_rv * vn;  g = yn - c_gu * un - c_gv * vn;  b = yn + c_bu * un;
// with the result in [0, 1].
struct GpuYuvConstants {
    float c_rv;
    float c_gu;
    float c_gv;
    float c_bu;
    float y_scale;
    float y_off;
    float c_off;
    float pad0; // free for the shader's own flag (the editor's linear output)
};
static_assert(sizeof(GpuYuvConstants) == 32);

struct YuvMatrixWeights {
    double kr;
    double kb;
};

// Duplicate of yuv_to_bgra.cpp's WeightsFor (file-local there). Unspecified
// means BT.709, because that is the only matrix this engine's encoder ever
// writes. test_edit_frame_gpu_converter.cpp pins the GPU output against the CPU
// converters, so the two tables cannot drift apart unnoticed.
[[nodiscard]] inline YuvMatrixWeights YuvWeightsFor(MatrixCoefficients matrix) noexcept {
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

// The float mirror of yuv_to_bgra.cpp's ComputeCoefs. Two differences, both
// deliberate:
//   - no 16.16 fixed-point shift (the shader is float already), and
//   - everything is divided by 255 at the end. ComputeCoefs' scales map the
//     normalised signal onto the 0..255 *output byte* domain; the shader wants
//     [0, 1] for its UNORM render target, and 255 is that domain's size
//     whatever the input bit depth. Dividing by the input's max code (1023 for
//     10-bit) instead would double-apply the bit-depth normalisation, which
//     ComputeCoefs already folds into y_scale/c_scale (255/876, 255/896, ...).
[[nodiscard]] inline GpuYuvConstants GpuYuvConstantsFor(MatrixCoefficients matrix, ColorRange range,
                                                        bool ten_bit) noexcept {
    const YuvMatrixWeights w = YuvWeightsFor(matrix);
    const double kg = 1.0 - w.kr - w.kb;
    const double rv = 2.0 * (1.0 - w.kr);
    const double bu = 2.0 * (1.0 - w.kb);
    const double gu = bu * w.kb / kg;
    const double gv = rv * w.kr / kg;

    const bool limited = range == ColorRange::Limited;

    double y_scale = 0.0;
    double c_scale = 0.0;
    double y_off = 0.0;
    double c_off = 0.0;
    if (ten_bit) {
        y_scale = limited ? (255.0 / 876.0) : (255.0 / 1023.0);
        c_scale = limited ? (255.0 / 896.0) : (255.0 / 1023.0);
        y_off = limited ? 64.0 : 0.0;
        c_off = 512.0;
    } else {
        y_scale = limited ? (255.0 / 219.0) : 1.0;
        c_scale = limited ? (255.0 / 224.0) : 1.0;
        y_off = limited ? 16.0 : 0.0;
        c_off = 128.0;
    }

    constexpr double kOutputDomain = 255.0;
    GpuYuvConstants c{};
    c.c_rv = static_cast<float>(rv * c_scale / kOutputDomain);
    c.c_gu = static_cast<float>(gu * c_scale / kOutputDomain);
    c.c_gv = static_cast<float>(gv * c_scale / kOutputDomain);
    c.c_bu = static_cast<float>(bu * c_scale / kOutputDomain);
    c.y_scale = static_cast<float>(y_scale / kOutputDomain);
    c.y_off = static_cast<float>(y_off);
    c.c_off = static_cast<float>(c_off);
    c.pad0 = 0.0f;
    return c;
}

} // namespace exosnap::engine
