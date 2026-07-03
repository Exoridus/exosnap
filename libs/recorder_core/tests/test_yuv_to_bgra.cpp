#include <gtest/gtest.h>

#include "yuv_to_bgra.h"

#include <cstdint>
#include <vector>

// Validates ConvertYuv420ToBgra (the fix for the pre-existing BT.601
// hard-coded snapshot conversion bug -- see yuv_to_bgra.h) against
// HAND-COMPUTED golden values derived from the ITU-R literature constants:
//
//   BT.709 (Rec. ITU-R BT.709-6):  R = Y' + 1.5748 Cr'
//                                  G = Y' - 0.18732 Cb' - 0.46812 Cr'
//                                  B = Y' + 1.8556 Cb'
//   BT.601 (Rec. ITU-R BT.601-7):  R = Y' + 1.402  Cr'
//                                  G = Y' - 0.34414 Cb' - 0.71414 Cr'
//                                  B = Y' + 1.772  Cb'
//
//   limited (studio) range 8-bit:  Y' = (Y-16)/219,  C' = (C-128)/224
//   full range 8-bit:              Y' = Y/255,       C' = (C-128)/255
//   limited range 10-bit:          Y' = (Y-64)/876,  C' = (C-512)/896
//   full range 10-bit:             Y' = Y/1023,      C' = (C-512)/1023
//
// The expected RGB bytes below were computed by hand from those formulas
// (multiply by 255, round). The test deliberately shares NO code or
// constants with the implementation: an earlier version re-derived expected
// values through a forward transform that duplicated the implementation's
// coefficient table, which would have silently passed if a coefficient were
// wrong in both places. Tolerance 1 covers the implementation's 16.16
// fixed-point rounding vs. exact-arithmetic rounding.

namespace {

using recorder_core::ColorRange;
using recorder_core::ConvertYuv420ToBgra;
using recorder_core::MatrixCoefficients;
using recorder_core::PlanarYuv420Frame;
using recorder_core::YuvToBgraParams;

struct Golden {
    // Inputs (one uniform 2x2 luma block + one chroma pair)
    uint16_t y;
    uint16_t cb;
    uint16_t cr;
    // Hand-computed expected output
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

// Converts a minimal uniform 2x2 NV12 frame and checks the result against
// the hand-computed golden pixel.
void ExpectGoldenNv12(const Golden& gold, MatrixCoefficients matrix, ColorRange range, int tolerance = 1) {
    const auto y8 = static_cast<uint8_t>(gold.y);
    const auto u8 = static_cast<uint8_t>(gold.cb);
    const auto v8 = static_cast<uint8_t>(gold.cr);
    std::vector<uint8_t> y_plane = {y8, y8, y8, y8};
    std::vector<uint8_t> uv_plane = {u8, v8};

    PlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = 2;
    src.uv_plane = uv_plane.data();
    src.uv_stride_bytes = 2;
    src.width = 2;
    src.height = 2;
    src.bits_per_sample = 8;

    YuvToBgraParams params;
    params.matrix = matrix;
    params.range = range;

    std::vector<uint8_t> out(2 * 2 * 4, 0);
    ConvertYuv420ToBgra(src, params, out.data(), 2 * 4);

    EXPECT_NEAR(static_cast<int>(out[2]), static_cast<int>(gold.r), tolerance)
        << "R for YUV(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_NEAR(static_cast<int>(out[1]), static_cast<int>(gold.g), tolerance)
        << "G for YUV(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_NEAR(static_cast<int>(out[0]), static_cast<int>(gold.b), tolerance)
        << "B for YUV(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_EQ(out[3], 255);
}

// Same for P010 (samples given as 10-bit values, packed into bits 15:6).
void ExpectGoldenP010(const Golden& gold, MatrixCoefficients matrix, ColorRange range, int tolerance = 1) {
    const auto pack = [](uint16_t v10) -> uint16_t { return static_cast<uint16_t>(v10 << 6); };
    std::vector<uint16_t> y_plane = {pack(gold.y), pack(gold.y), pack(gold.y), pack(gold.y)};
    std::vector<uint16_t> uv_plane = {pack(gold.cb), pack(gold.cr)};

    PlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(y_plane.data());
    src.y_stride_bytes = static_cast<uint32_t>(2 * sizeof(uint16_t));
    src.uv_plane = reinterpret_cast<const uint8_t*>(uv_plane.data());
    src.uv_stride_bytes = static_cast<uint32_t>(2 * sizeof(uint16_t));
    src.width = 2;
    src.height = 2;
    src.bits_per_sample = 10;

    YuvToBgraParams params;
    params.matrix = matrix;
    params.range = range;

    std::vector<uint8_t> out(2 * 2 * 4, 0);
    ConvertYuv420ToBgra(src, params, out.data(), 2 * 4);

    EXPECT_NEAR(static_cast<int>(out[2]), static_cast<int>(gold.r), tolerance)
        << "R for YUV10(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_NEAR(static_cast<int>(out[1]), static_cast<int>(gold.g), tolerance)
        << "G for YUV10(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_NEAR(static_cast<int>(out[0]), static_cast<int>(gold.b), tolerance)
        << "B for YUV10(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_EQ(out[3], 255);
}

} // namespace

// --- 8-bit golden vectors, all four matrix x range combinations -----------

TEST(YuvToBgra, GoldenBt709Limited) {
    // Y=16 -> Y'=0 (black), Y=235 -> Y'=1 (white), chroma neutral at 128.
    ExpectGoldenNv12({16, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
    ExpectGoldenNv12({235, 128, 128, 255, 255, 255}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
    // Y=126, Cr=180: Y'=110/219=0.50228, Cr'=52/224=0.23214
    //   R = 0.50228 + 1.5748*0.23214 = 0.86786 -> 221
    //   G = 0.50228 - 0.46812*0.23214 = 0.39361 -> 100
    //   B = 0.50228                              -> 128
    ExpectGoldenNv12({126, 128, 180, 221, 100, 128}, MatrixCoefficients::Bt709, ColorRange::Limited);
    // Y=126, Cb=180: B = 0.50228 + 1.8556*0.23214 = 0.93305 -> 238
    //                G = 0.50228 - 0.18732*0.23214 = 0.45880 -> 117
    ExpectGoldenNv12({126, 180, 128, 128, 117, 238}, MatrixCoefficients::Bt709, ColorRange::Limited);
}

TEST(YuvToBgra, GoldenBt601Limited) {
    ExpectGoldenNv12({16, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt601, ColorRange::Limited, 0);
    ExpectGoldenNv12({235, 128, 128, 255, 255, 255}, MatrixCoefficients::Bt601, ColorRange::Limited, 0);
    // Y=126, Cr=202: Y'=0.50228, Cr'=74/224=0.33036
    //   R = 0.50228 + 1.402*0.33036 = 0.96544  -> 246
    //   G = 0.50228 - 0.71414*0.33036 = 0.26636 -> 68
    ExpectGoldenNv12({126, 128, 202, 246, 68, 128}, MatrixCoefficients::Bt601, ColorRange::Limited);
    // Y=126, Cr=180: R = 0.50228 + 1.402*0.23214 = 0.82775 -> 211
    //                G = 0.50228 - 0.71414*0.23214 = 0.33650 -> 86
    ExpectGoldenNv12({126, 128, 180, 211, 86, 128}, MatrixCoefficients::Bt601, ColorRange::Limited);
    // Y=126, Cb=180: B = 0.50228 + 1.772*0.23214 = 0.91364 -> 233
    //                G = 0.50228 - 0.34414*0.23214 = 0.42239 -> 108
    ExpectGoldenNv12({126, 180, 128, 128, 108, 233}, MatrixCoefficients::Bt601, ColorRange::Limited);
}

TEST(YuvToBgra, GoldenBt709Full) {
    ExpectGoldenNv12({0, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    ExpectGoldenNv12({255, 128, 128, 255, 255, 255}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    // Y=128, Cr=180: Y'=128/255=0.50196, Cr'=52/255=0.20392
    //   R = 0.50196 + 1.5748*0.20392 = 0.82310 -> 210
    //   G = 0.50196 - 0.46812*0.20392 = 0.40650 -> 104
    ExpectGoldenNv12({128, 128, 180, 210, 104, 128}, MatrixCoefficients::Bt709, ColorRange::Full);
    // Y=128, Cb=180: B = 0.50196 + 1.8556*0.20392 = 0.88036 -> 224
    //                G = 0.50196 - 0.18732*0.20392 = 0.46376 -> 118
    ExpectGoldenNv12({128, 180, 128, 128, 118, 224}, MatrixCoefficients::Bt709, ColorRange::Full);
}

TEST(YuvToBgra, GoldenBt601Full) {
    ExpectGoldenNv12({0, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt601, ColorRange::Full, 0);
    ExpectGoldenNv12({255, 128, 128, 255, 255, 255}, MatrixCoefficients::Bt601, ColorRange::Full, 0);
    // Y=128, Cr=180: R = 0.50196 + 1.402*0.20392 = 0.78786 -> 201
    //                G = 0.50196 - 0.71414*0.20392 = 0.35633 -> 91
    ExpectGoldenNv12({128, 128, 180, 201, 91, 128}, MatrixCoefficients::Bt601, ColorRange::Full);
    // Y=128, Cb=180: B = 0.50196 + 1.772*0.20392 = 0.86331 -> 220
    //                G = 0.50196 - 0.34414*0.20392 = 0.43178 -> 110
    ExpectGoldenNv12({128, 180, 128, 128, 110, 220}, MatrixCoefficients::Bt601, ColorRange::Full);
}

// --- P010 (10-bit) golden vectors ------------------------------------------

TEST(YuvToBgra, GoldenP010Bt709Limited) {
    // 10-bit studio swing: black Y=64, white Y=940, chroma neutral 512.
    ExpectGoldenP010({64, 512, 512, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
    ExpectGoldenP010({940, 512, 512, 255, 255, 255}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
    // Exactly 4x the 8-bit (126,128,180) vector: Y'=440/876=0.50228,
    // Cr'=208/896=0.23214 -- identical normalized values, so the expected
    // RGB matches the 8-bit golden pixel (221,100,128).
    ExpectGoldenP010({504, 512, 720, 221, 100, 128}, MatrixCoefficients::Bt709, ColorRange::Limited);
}

TEST(YuvToBgra, GoldenP010Bt709Full) {
    ExpectGoldenP010({0, 512, 512, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    ExpectGoldenP010({1023, 512, 512, 255, 255, 255}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    // Y=512, Cr=720: Y'=512/1023=0.50049, Cr'=208/1023=0.20332
    //   R = 0.50049 + 1.5748*0.20332 = 0.82068 -> 209
    //   G = 0.50049 - 0.46812*0.20332 = 0.40531 -> 103
    ExpectGoldenP010({512, 512, 720, 209, 103, 128}, MatrixCoefficients::Bt709, ColorRange::Full);
}

TEST(YuvToBgra, GoldenP010Bt601Full) {
    // Y=512, Cr=720: R = 0.50049 + 1.402*0.20332 = 0.78554 -> 200
    //                G = 0.50049 - 0.71414*0.20332 = 0.35529 -> 91
    ExpectGoldenP010({512, 512, 720, 200, 91, 128}, MatrixCoefficients::Bt601, ColorRange::Full);
}

// --- Addressing coverage: non-uniform frames, every pixel checked ----------

// Distinct luma per pixel on a padded-stride 4x4 grayscale frame (BT.709
// full, neutral chroma): expected output is exactly R=G=B=Y per pixel. This
// pins down row/column addressing and the Y-stride handling -- a swapped
// index or ignored stride produces the wrong Y somewhere in the grid.
TEST(YuvToBgra, NonUniformLumaEveryPixelWithPaddedStrides) {
    constexpr uint32_t kW = 4, kH = 4;
    constexpr uint32_t kYStride = 8;    // 4 bytes padding per row
    constexpr uint32_t kUvStride = 8;   // 4 bytes padding per row
    constexpr uint32_t kOutStride = 20; // 4 bytes padding per row

    std::vector<uint8_t> y_plane(kYStride * kH, 0xEE); // poison the padding
    for (uint32_t r = 0; r < kH; ++r) {
        for (uint32_t c = 0; c < kW; ++c) {
            y_plane[r * kYStride + c] = static_cast<uint8_t>(10 + r * 60 + c * 10);
        }
    }
    std::vector<uint8_t> uv_plane(kUvStride * (kH / 2), 128); // neutral chroma

    PlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = kYStride;
    src.uv_plane = uv_plane.data();
    src.uv_stride_bytes = kUvStride;
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 8;

    YuvToBgraParams params;
    params.matrix = MatrixCoefficients::Bt709;
    params.range = ColorRange::Full;

    std::vector<uint8_t> out(kOutStride * kH, 0);
    ConvertYuv420ToBgra(src, params, out.data(), kOutStride);

    for (uint32_t r = 0; r < kH; ++r) {
        for (uint32_t c = 0; c < kW; ++c) {
            const int expected = 10 + static_cast<int>(r) * 60 + static_cast<int>(c) * 10;
            const uint8_t* px = out.data() + r * kOutStride + c * 4;
            EXPECT_EQ(static_cast<int>(px[0]), expected) << "B at (" << r << "," << c << ")";
            EXPECT_EQ(static_cast<int>(px[1]), expected) << "G at (" << r << "," << c << ")";
            EXPECT_EQ(static_cast<int>(px[2]), expected) << "R at (" << r << "," << c << ")";
            EXPECT_EQ(px[3], 255);
        }
    }
}

// Four distinct chroma 2x2 blocks on a 4x4 frame (BT.709 full, uniform
// Y=128): every pixel must resolve to its own block's hand-computed golden
// color. This pins down the 4:2:0 UV subsampling addressing (row/2, col&~1,
// U-before-V interleave) -- any mixup colors some quadrant wrongly.
TEST(YuvToBgra, NonUniformChromaBlocksEveryPixel) {
    constexpr uint32_t kW = 4, kH = 4;

    std::vector<uint8_t> y_plane(kW * kH, 128);
    // UV rows: 2 chroma rows x 2 chroma columns, interleaved U,V:
    //   block (0,0): Cb=128, Cr=180 -> (210, 104, 128)   [golden above]
    //   block (0,1): Cb=180, Cr=128 -> (128, 118, 224)   [golden above]
    //   block (1,0): Cb=128, Cr=128 -> (128, 128, 128)   [neutral gray]
    //   block (1,1): Cb=180, Cr=180 -> R = 0.50196+1.5748*0.20392            = 0.82310 -> 210
    //                                  G = 0.50196-(0.18732+0.46812)*0.20392 = 0.36830 -> 94
    //                                  B = 0.50196+1.8556*0.20392            = 0.88036 -> 224
    const std::vector<uint8_t> uv_plane = {
        128, 180, 180, 128, // chroma row 0: (U,V) (U,V)
        128, 128, 180, 180, // chroma row 1
    };
    struct Rgb {
        int r, g, b;
    };
    const Rgb expected_blocks[2][2] = {
        {{210, 104, 128}, {128, 118, 224}},
        {{128, 128, 128}, {210, 94, 224}},
    };

    PlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = kW;
    src.uv_plane = uv_plane.data();
    src.uv_stride_bytes = kW;
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 8;

    YuvToBgraParams params;
    params.matrix = MatrixCoefficients::Bt709;
    params.range = ColorRange::Full;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertYuv420ToBgra(src, params, out.data(), kW * 4);

    for (uint32_t r = 0; r < kH; ++r) {
        for (uint32_t c = 0; c < kW; ++c) {
            const Rgb& want = expected_blocks[r / 2][c / 2];
            const uint8_t* px = out.data() + (r * kW + c) * 4;
            EXPECT_NEAR(static_cast<int>(px[2]), want.r, 1) << "R at (" << r << "," << c << ")";
            EXPECT_NEAR(static_cast<int>(px[1]), want.g, 1) << "G at (" << r << "," << c << ")";
            EXPECT_NEAR(static_cast<int>(px[0]), want.b, 1) << "B at (" << r << "," << c << ")";
            EXPECT_EQ(px[3], 255);
        }
    }
}

// Proves the matrix parameter is load-bearing without any shared constants:
// the SAME YUV input decoded with BT.709 vs BT.601 must land on the two
// different hand-computed golden colors (221 vs 211 in R, 100 vs 86 in G).
TEST(YuvToBgra, MatrixParameterSelectsDifferentGoldenColors) {
    ExpectGoldenNv12({126, 128, 180, 221, 100, 128}, MatrixCoefficients::Bt709, ColorRange::Limited);
    ExpectGoldenNv12({126, 128, 180, 211, 86, 128}, MatrixCoefficients::Bt601, ColorRange::Limited);
}

// Proves the range parameter is load-bearing: full-range decode of studio
// black (Y=16) must NOT be black (16/255 -> 16), while limited decode maps
// it to exact black.
TEST(YuvToBgra, RangeParameterChangesBlackLevel) {
    ExpectGoldenNv12({16, 128, 128, 16, 16, 16}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    ExpectGoldenNv12({16, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
}

TEST(YuvToBgra, DegenerateInputsAreNoOps) {
    std::vector<uint8_t> out(16, 0xAB);
    PlanarYuv420Frame src; // all zero/null by default
    YuvToBgraParams params;
    ConvertYuv420ToBgra(src, params, out.data(), 4);
    for (uint8_t b : out) {
        EXPECT_EQ(b, 0xAB);
    }
}
