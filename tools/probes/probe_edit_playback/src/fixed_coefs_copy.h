#pragma once

// Private copy of the engine's fixed-point YUV->BGRA coefficient math
// (libs/engine/src/yuv_to_bgra.cpp, anonymous namespace) for the SIMD
// measurement probe (step G, 2026-08-01 SIMD-vs-scalar investigation).
//
// MEASUREMENT ONLY -- production code is untouched. This exists purely so
// the auto-vectorized and hand-written SIMD variants living in this probe's
// own translation units use bit-identical coefficient math to the real
// exosnap::engine::ConvertFullPlanarYuv420ToBgra baseline, which is required
// for the pixel-exact correctness comparison in step G to mean anything.
//
// Kept as `inline` functions (not an anonymous namespace) so this header can
// be included from multiple probe .cpp files without violating ODR.

#include <exosnap/engine/color_metadata.h>

#include <cstdint>

namespace probe_g {

struct MatrixWeights {
    double kr;
    double kb;
};

// Mirrors yuv_to_bgra.cpp's WeightsFor() exactly.
inline MatrixWeights WeightsFor(exosnap::engine::MatrixCoefficients matrix) noexcept {
    using exosnap::engine::MatrixCoefficients;
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

// Mirrors yuv_to_bgra.cpp's ComputeCoefs() exactly.
inline FixedCoefs ComputeCoefs(exosnap::engine::MatrixCoefficients matrix, exosnap::engine::ColorRange range,
                                uint32_t bits_per_sample) noexcept {
    const MatrixWeights w = WeightsFor(matrix);
    const double kg = 1.0 - w.kr - w.kb;
    const double rv = 2.0 * (1.0 - w.kr);
    const double bu = 2.0 * (1.0 - w.kb);
    const double gu = bu * w.kb / kg;
    const double gv = rv * w.kr / kg;

    const bool ten_bit = bits_per_sample > 8;
    const bool limited = range == exosnap::engine::ColorRange::Limited;

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

// Mirrors yuv_to_bgra.cpp's ClampFixedToByte() exactly.
inline uint8_t ClampFixedToByte(int32_t fixed) noexcept {
    const int32_t v = (fixed + kFixedRound) >> kFixedShift;
    if (v <= 0)
        return 0;
    if (v >= 255)
        return 255;
    return static_cast<uint8_t>(v);
}

} // namespace probe_g
