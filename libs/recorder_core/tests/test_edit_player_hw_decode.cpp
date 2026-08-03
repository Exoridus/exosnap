#include <gtest/gtest.h>

#include "edit_player_hw_decode.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <cstring>
#include <memory>

// Pure logic (no D3D11/no decoder): constructs synthetic semi-planar frames
// by hand and checks DeinterleaveHwReadbackFrame's output planes, so these
// run on any host without a hardware adapter or a fixture clip.

namespace {

using recorder_core::DeinterleaveHwReadbackFrame;
using recorder_core::IsSupportedHwReadbackFormat;

struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept {
        av_frame_free(&f);
    }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

FramePtr MakeFrame(int format, int width, int height) {
    AVFrame* f = av_frame_alloc();
    f->format = format;
    f->width = width;
    f->height = height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    return FramePtr(f);
}

} // namespace

TEST(EditPlayerHwDecode, RecognizesNv12AndP010AsSupported) {
    EXPECT_TRUE(IsSupportedHwReadbackFormat(AV_PIX_FMT_NV12));
    EXPECT_TRUE(IsSupportedHwReadbackFormat(AV_PIX_FMT_P010LE));
    EXPECT_FALSE(IsSupportedHwReadbackFormat(AV_PIX_FMT_YUV420P));
    EXPECT_FALSE(IsSupportedHwReadbackFormat(AV_PIX_FMT_D3D11));
}

TEST(EditPlayerHwDecode, RejectsAlreadyPlanarSource) {
    FramePtr src = MakeFrame(AV_PIX_FMT_YUV420P, 4, 2);
    ASSERT_NE(src, nullptr);

    AVFrame* out = DeinterleaveHwReadbackFrame(src.get());

    EXPECT_EQ(out, nullptr);
}

TEST(EditPlayerHwDecode, DeinterleavesNv12ChromaWithoutRescale) {
    // 4x2 luma, 2x1 chroma. Semi-planar UV: U0 V0 U1 V1 per chroma row.
    FramePtr src = MakeFrame(AV_PIX_FMT_NV12, 4, 2);
    ASSERT_NE(src, nullptr);

    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            src->data[0][static_cast<size_t>(y) * src->linesize[0] + x] = static_cast<uint8_t>(10 + y * 4 + x);
    src->data[1][0] = 100; // U0
    src->data[1][1] = 150; // V0
    src->data[1][2] = 110; // U1
    src->data[1][3] = 160; // V1

    FramePtr out(DeinterleaveHwReadbackFrame(src.get()));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->format, AV_PIX_FMT_YUV420P);
    EXPECT_EQ(out->width, 4);
    EXPECT_EQ(out->height, 2);

    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(out->data[0][static_cast<size_t>(y) * out->linesize[0] + x], 10 + y * 4 + x)
                << "y=" << y << " x=" << x;

    EXPECT_EQ(out->data[1][0], 100); // U0, unchanged (8-bit, no P010 shift)
    EXPECT_EQ(out->data[1][1], 110); // U1
    EXPECT_EQ(out->data[2][0], 150); // V0
    EXPECT_EQ(out->data[2][1], 160); // V1
}

TEST(EditPlayerHwDecode, RescalesP010LeftJustificationOnLumaAndChroma) {
    // 4x2 luma, 2x1 chroma, all samples P010-left-justified (code << 6).
    FramePtr src = MakeFrame(AV_PIX_FMT_P010LE, 4, 2);
    ASSERT_NE(src, nullptr);

    const uint16_t luma_codes[2][4] = {{64, 128, 512, 940}, {64, 128, 512, 940}};
    for (int y = 0; y < 2; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(src->data[0] + static_cast<size_t>(y) * src->linesize[0]);
        for (int x = 0; x < 4; ++x)
            row[x] = static_cast<uint16_t>(luma_codes[y][x] << 6);
    }
    auto* uv = reinterpret_cast<uint16_t*>(src->data[1]);
    uv[0] = static_cast<uint16_t>(512 << 6); // U0
    uv[1] = static_cast<uint16_t>(600 << 6); // V0
    uv[2] = static_cast<uint16_t>(400 << 6); // U1
    uv[3] = static_cast<uint16_t>(700 << 6); // V1

    FramePtr out(DeinterleaveHwReadbackFrame(src.get()));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->format, AV_PIX_FMT_YUV420P10LE);

    for (int y = 0; y < 2; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(out->data[0] + static_cast<size_t>(y) * out->linesize[0]);
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(row[x], luma_codes[y][x]) << "y=" << y << " x=" << x
                                                << " -- must be a plain 10-bit code, "
                                                   "not left-justified into the 16-bit container";
    }

    const auto* u_row = reinterpret_cast<const uint16_t*>(out->data[1]);
    const auto* v_row = reinterpret_cast<const uint16_t*>(out->data[2]);
    EXPECT_EQ(u_row[0], 512);
    EXPECT_EQ(u_row[1], 400);
    EXPECT_EQ(v_row[0], 600);
    EXPECT_EQ(v_row[1], 700);
}

TEST(EditPlayerHwDecode, RejectsNullAndNonPositiveDimensions) {
    EXPECT_EQ(DeinterleaveHwReadbackFrame(nullptr), nullptr);

    FramePtr src = MakeFrame(AV_PIX_FMT_NV12, 4, 2);
    ASSERT_NE(src, nullptr);
    src->width = 0;
    EXPECT_EQ(DeinterleaveHwReadbackFrame(src.get()), nullptr);
}
