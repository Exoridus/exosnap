#include <gtest/gtest.h>

#include "yuv_to_bgra.h"

#include <array>
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
using recorder_core::ConvertAyuvToBgra;
using recorder_core::ConvertFullPlanarYuv420ToBgra;
using recorder_core::ConvertYuv420ToBgra;
using recorder_core::FullPlanarYuv420Frame;
using recorder_core::MatrixCoefficients;
using recorder_core::PackedAyuvFrame;
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

// Converts a minimal uniform 2x2 packed-AYUV frame and checks the result
// against the hand-computed golden pixel. Packed AYUV is single-plane, 4 bytes
// per pixel, memory byte order [V, U, Y, A] (DXGI_FORMAT_AYUV) — spelled out
// explicitly below so a byte-order regression is caught. Because 4:4:4 carries
// full chroma per pixel, the golden RGB is identical to the NV12 vector with
// the same Y/U/V samples.
void ExpectGoldenAyuv(const Golden& gold, MatrixCoefficients matrix, ColorRange range, int tolerance = 1) {
    const auto y8 = static_cast<uint8_t>(gold.y);
    const auto u8 = static_cast<uint8_t>(gold.cb);
    const auto v8 = static_cast<uint8_t>(gold.cr);
    // Four identical pixels, each 4 bytes in [V, U, Y, A] order. Alpha input is
    // deliberately NOT 0xFF (0x11) to prove the decoder ignores it and writes
    // 0xFF out.
    const std::array<uint8_t, 4> px = {v8, u8, y8, 0x11};
    std::vector<uint8_t> data;
    for (int i = 0; i < 4; ++i)
        data.insert(data.end(), px.begin(), px.end());

    PackedAyuvFrame src;
    src.data = data.data();
    src.stride_bytes = 2 * 4; // 2 px/row, 4 bytes/px, no padding
    src.width = 2;
    src.height = 2;

    YuvToBgraParams params;
    params.matrix = matrix;
    params.range = range;

    std::vector<uint8_t> out(2 * 2 * 4, 0);
    ConvertAyuvToBgra(src, params, out.data(), 2 * 4);

    EXPECT_NEAR(static_cast<int>(out[2]), static_cast<int>(gold.r), tolerance)
        << "R for AYUV(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_NEAR(static_cast<int>(out[1]), static_cast<int>(gold.g), tolerance)
        << "G for AYUV(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
    EXPECT_NEAR(static_cast<int>(out[0]), static_cast<int>(gold.b), tolerance)
        << "B for AYUV(" << gold.y << "," << gold.cb << "," << gold.cr << ")";
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

// --- Packed AYUV (4:4:4, 8-bit) golden vectors -----------------------------
// Same matrix/range math as NV12, no chroma subsampling. The expected RGB
// reuses the hand-computed NV12 goldens above (identical Y/U/V samples).

TEST(AyuvToBgra, GoldenBt709Limited) {
    // Y=16 -> black, Y=235 -> white, neutral chroma 128.
    ExpectGoldenAyuv({16, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
    ExpectGoldenAyuv({235, 128, 128, 255, 255, 255}, MatrixCoefficients::Bt709, ColorRange::Limited, 0);
    // Saturated Cr: (126,128,180) -> (221,100,128), same as the NV12 golden.
    ExpectGoldenAyuv({126, 128, 180, 221, 100, 128}, MatrixCoefficients::Bt709, ColorRange::Limited);
    // Saturated Cb: (126,180,128) -> (128,117,238).
    ExpectGoldenAyuv({126, 180, 128, 128, 117, 238}, MatrixCoefficients::Bt709, ColorRange::Limited);
}

TEST(AyuvToBgra, GoldenBt709Full) {
    ExpectGoldenAyuv({0, 128, 128, 0, 0, 0}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    ExpectGoldenAyuv({255, 128, 128, 255, 255, 255}, MatrixCoefficients::Bt709, ColorRange::Full, 0);
    // (128,128,180) -> (210,104,128); (128,180,128) -> (128,118,224).
    ExpectGoldenAyuv({128, 128, 180, 210, 104, 128}, MatrixCoefficients::Bt709, ColorRange::Full);
    ExpectGoldenAyuv({128, 180, 128, 128, 118, 224}, MatrixCoefficients::Bt709, ColorRange::Full);
}

// The alpha input byte (0x11 in the helper) must be ignored; output alpha 0xFF.
// (Already asserted per-pixel by the helper; this names the intent.)
TEST(AyuvToBgra, IgnoresSourceAlphaWritesOpaque) {
    ExpectGoldenAyuv({126, 128, 180, 221, 100, 128}, MatrixCoefficients::Bt709, ColorRange::Limited);
}

// Padded stride: a 2x2 frame whose row pitch exceeds width*4. Distinct pixels
// per position pin down both the packed [V,U,Y] addressing and stride handling.
// BT.709 full, neutral chroma -> R=G=B=Y, so each pixel decodes to its own Y.
TEST(AyuvToBgra, PaddedStrideDistinctPixels) {
    constexpr uint32_t kW = 2, kH = 2;
    constexpr uint32_t kStride = kW * 4 + 8; // 8 bytes padding per row
    const uint8_t lumas[kH][kW] = {{40, 90}, {150, 220}};

    std::vector<uint8_t> data(kStride * kH, 0xEE); // poison the padding
    for (uint32_t r = 0; r < kH; ++r) {
        for (uint32_t c = 0; c < kW; ++c) {
            uint8_t* p = data.data() + r * kStride + c * 4;
            p[0] = 128;         // V (neutral)
            p[1] = 128;         // U (neutral)
            p[2] = lumas[r][c]; // Y
            p[3] = 0x00;        // A (ignored)
        }
    }

    PackedAyuvFrame src;
    src.data = data.data();
    src.stride_bytes = kStride;
    src.width = kW;
    src.height = kH;

    YuvToBgraParams params;
    params.matrix = MatrixCoefficients::Bt709;
    params.range = ColorRange::Full;

    constexpr uint32_t kOutStride = kW * 4 + 4; // also padded
    std::vector<uint8_t> out(kOutStride * kH, 0);
    ConvertAyuvToBgra(src, params, out.data(), kOutStride);

    for (uint32_t r = 0; r < kH; ++r) {
        for (uint32_t c = 0; c < kW; ++c) {
            const int expected = lumas[r][c];
            const uint8_t* px = out.data() + r * kOutStride + c * 4;
            EXPECT_NEAR(static_cast<int>(px[0]), expected, 1) << "B at (" << r << "," << c << ")";
            EXPECT_NEAR(static_cast<int>(px[1]), expected, 1) << "G at (" << r << "," << c << ")";
            EXPECT_NEAR(static_cast<int>(px[2]), expected, 1) << "R at (" << r << "," << c << ")";
            EXPECT_EQ(px[3], 255);
        }
    }
}

TEST(AyuvToBgra, DegenerateInputsAreNoOps) {
    std::vector<uint8_t> out(16, 0xAB);
    PackedAyuvFrame src; // all zero/null by default
    YuvToBgraParams params;
    ConvertAyuvToBgra(src, params, out.data(), 4);
    for (uint8_t b : out)
        EXPECT_EQ(b, 0xAB);
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

// --- Fully-planar YUV420 (separate Y/U/V planes -- FFmpeg AV_PIX_FMT_YUV420P /
// YUV420P10LE software-decoder output) golden vectors. Reuses the exact NV12
// goldens above -- same normalized Y'/Cb'/Cr' values, different memory layout.

TEST(FullPlanarYuv420ToBgra, GoldenBt709Limited8Bit) {
    // Same golden pixel as GoldenBt709Limited's third case: Y=126,Cr=180 -> (221,100,128).
    constexpr uint32_t kW = 2, kH = 2;
    std::vector<uint8_t> y_plane(kW * kH, 126);
    std::vector<uint8_t> u_plane(1, 128); // 2x2 luma -> 1x1 chroma
    std::vector<uint8_t> v_plane(1, 180);

    FullPlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = kW;
    src.u_plane = u_plane.data();
    src.u_stride_bytes = 1;
    src.v_plane = v_plane.data();
    src.v_stride_bytes = 1;
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 8;

    recorder_core::YuvToBgraParams params;
    params.matrix = recorder_core::MatrixCoefficients::Bt709;
    params.range = recorder_core::ColorRange::Limited;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), kW * 4);

    for (uint32_t i = 0; i < kW * kH; ++i) {
        const uint8_t* px = out.data() + i * 4;
        EXPECT_NEAR(static_cast<int>(px[2]), 221, 1) << "R at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[1]), 100, 1) << "G at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[0]), 128, 1) << "B at pixel " << i;
        EXPECT_EQ(px[3], 255);
    }
}

TEST(FullPlanarYuv420ToBgra, GoldenBt709Limited10Bit) {
    // Same golden as GoldenP010Bt709Limited's third case: Y=504,Cr=720 -> (221,100,128),
    // but as 16-bit little-endian words with the FULL 10-bit value (no <<6 left-justify --
    // that packing is P010-specific; planar YUV420P10LE stores the plain 10-bit value).
    constexpr uint32_t kW = 2, kH = 2;
    std::vector<uint16_t> y_plane(kW * kH, 504);
    std::vector<uint16_t> u_plane(1, 512);
    std::vector<uint16_t> v_plane(1, 720);

    FullPlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(y_plane.data());
    src.y_stride_bytes = static_cast<uint32_t>(kW * sizeof(uint16_t));
    src.u_plane = reinterpret_cast<const uint8_t*>(u_plane.data());
    src.u_stride_bytes = static_cast<uint32_t>(sizeof(uint16_t));
    src.v_plane = reinterpret_cast<const uint8_t*>(v_plane.data());
    src.v_stride_bytes = static_cast<uint32_t>(sizeof(uint16_t));
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 10;

    recorder_core::YuvToBgraParams params;
    params.matrix = recorder_core::MatrixCoefficients::Bt709;
    params.range = recorder_core::ColorRange::Limited;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), kW * 4);

    for (uint32_t i = 0; i < kW * kH; ++i) {
        const uint8_t* px = out.data() + i * 4;
        EXPECT_NEAR(static_cast<int>(px[2]), 221, 1) << "R at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[1]), 100, 1) << "G at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[0]), 128, 1) << "B at pixel " << i;
        EXPECT_EQ(px[3], 255);
    }
}

TEST(FullPlanarYuv420ToBgra, NonUniformChromaBlocksEveryPixel) {
    // Same 4x4 four-quadrant scenario as YuvToBgra.NonUniformChromaBlocksEveryPixel,
    // re-expressed with separate U/V planes instead of interleaved UV.
    constexpr uint32_t kW = 4, kH = 4;
    std::vector<uint8_t> y_plane(kW * kH, 128);
    // 2x2 chroma grid (one sample per 2x2 luma block).
    const std::vector<uint8_t> u_plane = {128, 180, 128, 180};
    const std::vector<uint8_t> v_plane = {180, 128, 128, 180};
    struct Rgb {
        int r, g, b;
    };
    const Rgb expected_blocks[2][2] = {
        {{210, 104, 128}, {128, 118, 224}},
        {{128, 128, 128}, {210, 94, 224}},
    };

    FullPlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = kW;
    src.u_plane = u_plane.data();
    src.u_stride_bytes = kW / 2;
    src.v_plane = v_plane.data();
    src.v_stride_bytes = kW / 2;
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 8;

    recorder_core::YuvToBgraParams params;
    params.matrix = recorder_core::MatrixCoefficients::Bt709;
    params.range = recorder_core::ColorRange::Full;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), kW * 4);

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

TEST(FullPlanarYuv420ToBgra, DegenerateInputsAreNoOps) {
    std::vector<uint8_t> out(16, 0xAB);
    FullPlanarYuv420Frame src; // all zero/null by default
    recorder_core::YuvToBgraParams params;
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), 4);
    for (uint8_t b : out)
        EXPECT_EQ(b, 0xAB);
}

// --- SIMD path equals the scalar reference, byte for byte -------------------
//
// The 8-bit path has a hand-written SSE4.1 implementation because the scalar
// one is the editor player's dominant per-frame cost. It is only ever
// acceptable if it is INDISTINGUISHABLE from the reference, so that is
// asserted directly rather than through the dispatcher (which on any given
// machine only ever runs one of the two).
namespace {

// Deterministic pseudo-random planes, deliberately including the extremes
// (0 and 255) that drive the conversion into its clamping branches.
struct ReferenceFrame {
    std::vector<uint8_t> y, u, v;
    recorder_core::FullPlanarYuv420Frame src;
};

ReferenceFrame MakeReferenceFrame(uint32_t width, uint32_t height) {
    const uint32_t chroma_w = (width + 1u) / 2u;
    const uint32_t chroma_h = (height + 1u) / 2u;
    ReferenceFrame f;
    f.y.resize(static_cast<size_t>(width) * height);
    f.u.resize(static_cast<size_t>(chroma_w) * chroma_h);
    f.v.resize(f.u.size());
    uint32_t state = 0x12345678u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<uint8_t>(state >> 24);
    };
    for (size_t i = 0; i < f.y.size(); ++i)
        f.y[i] = (i % 97u == 0) ? 0u : ((i % 89u == 0) ? 255u : next());
    for (size_t i = 0; i < f.u.size(); ++i) {
        f.u[i] = (i % 41u == 0) ? 0u : ((i % 37u == 0) ? 255u : next());
        f.v[i] = (i % 43u == 0) ? 255u : ((i % 31u == 0) ? 0u : next());
    }
    f.src.y_plane = f.y.data();
    f.src.y_stride_bytes = width;
    f.src.u_plane = f.u.data();
    f.src.u_stride_bytes = chroma_w;
    f.src.v_plane = f.v.data();
    f.src.v_stride_bytes = chroma_w;
    f.src.width = width;
    f.src.height = height;
    f.src.bits_per_sample = 8;
    return f;
}

} // namespace

TEST(FullPlanarYuv420ToBgraSimd, MatchesTheScalarReferenceByteForByte) {
    if (!recorder_core::CpuSupportsYuvToBgraSimd())
        GTEST_SKIP() << "CPU lacks SSE4.1; the dispatcher never selects the SIMD path here";

    // Widths straddling the 8-pixel vector width: exact multiples, remainders
    // that leave a partial block, and odd widths where the last chroma pair
    // covers a single pixel.
    for (const uint32_t width : {8u, 16u, 24u, 9u, 11u, 15u, 17u, 2u, 6u}) {
        for (const uint32_t height : {2u, 5u}) {
            for (const auto matrix :
                 {recorder_core::MatrixCoefficients::Bt709, recorder_core::MatrixCoefficients::Bt601,
                  recorder_core::MatrixCoefficients::Bt2020Ncl}) {
                for (const auto range : {recorder_core::ColorRange::Limited, recorder_core::ColorRange::Full}) {
                    const ReferenceFrame f = MakeReferenceFrame(width, height);
                    recorder_core::YuvToBgraParams params;
                    params.matrix = matrix;
                    params.range = range;

                    const uint32_t stride = width * 4u;
                    std::vector<uint8_t> expected(static_cast<size_t>(stride) * height, 0u);
                    std::vector<uint8_t> actual(expected.size(), 0u);
                    recorder_core::ConvertFullPlanarYuv420ToBgraScalar(f.src, params, expected.data(), stride);
                    recorder_core::ConvertFullPlanarYuv420ToBgraSimd(f.src, params, actual.data(), stride);

                    ASSERT_EQ(actual, expected)
                        << "SIMD output diverged at width=" << width << " height=" << height
                        << " matrix=" << static_cast<int>(matrix) << " range=" << static_cast<int>(range);
                }
            }
        }
    }
}

TEST(FullPlanarYuv420ToBgraSimd, TenBitInputGoesThroughTheScalarReference) {
    // The vectorised kernel covers 8-bit only; 10-bit must still convert
    // correctly rather than silently produce nothing.
    constexpr uint32_t kWidth = 8;
    constexpr uint32_t kHeight = 2;
    std::vector<uint16_t> y(static_cast<size_t>(kWidth) * kHeight, 600u);
    std::vector<uint16_t> u((kWidth / 2u) * (kHeight / 2u), 512u);
    std::vector<uint16_t> v(u.size(), 512u);

    recorder_core::FullPlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(y.data());
    src.y_stride_bytes = kWidth * 2u;
    src.u_plane = reinterpret_cast<const uint8_t*>(u.data());
    src.u_stride_bytes = (kWidth / 2u) * 2u;
    src.v_plane = reinterpret_cast<const uint8_t*>(v.data());
    src.v_stride_bytes = src.u_stride_bytes;
    src.width = kWidth;
    src.height = kHeight;
    src.bits_per_sample = 10;

    recorder_core::YuvToBgraParams params;
    const uint32_t stride = kWidth * 4u;
    std::vector<uint8_t> expected(static_cast<size_t>(stride) * kHeight, 0u);
    std::vector<uint8_t> actual(expected.size(), 0u);
    recorder_core::ConvertFullPlanarYuv420ToBgraScalar(src, params, expected.data(), stride);
    recorder_core::ConvertFullPlanarYuv420ToBgraSimd(src, params, actual.data(), stride);
    EXPECT_EQ(actual, expected);
    EXPECT_NE(actual[0], 0u) << "a mid-grey 10-bit frame must not convert to black";
}
