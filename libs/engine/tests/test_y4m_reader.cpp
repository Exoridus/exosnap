#include "../src/y4m_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

namespace exosnap::engine {
namespace {

TEST(ParseY4mHeader, ParsesWidthHeightFpsAndHeaderLength) {
    const std::string data = "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420jpeg\nFRAME\n";
    std::string err;
    const auto header = ParseY4mHeader(data, err);
    ASSERT_TRUE(header.has_value()) << err;
    EXPECT_EQ(header->width, 1920u);
    EXPECT_EQ(header->height, 1080u);
    EXPECT_EQ(header->fps_num, 30u);
    EXPECT_EQ(header->fps_den, 1u);
    // "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420jpeg\n" is 45 bytes.
    EXPECT_EQ(header->header_bytes, 45u);
}

TEST(ParseY4mHeader, AcceptsTagsInAnyOrder) {
    const std::string data = "YUV4MPEG2 C420mpeg2 H480 F60:1 W640\n";
    std::string err;
    const auto header = ParseY4mHeader(data, err);
    ASSERT_TRUE(header.has_value()) << err;
    EXPECT_EQ(header->width, 640u);
    EXPECT_EQ(header->height, 480u);
    EXPECT_EQ(header->fps_num, 60u);
}

TEST(ParseY4mHeader, RejectsWrongMagic) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader("NOTYUV4MPEG2 W1 H1 F1:1 C420\n", err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(ParseY4mHeader, RejectsMissingHeaderTerminator) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader("YUV4MPEG2 W1 H1 F1:1 C420", err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(ParseY4mHeader, RejectsUnsupportedChromaFormat) {
    std::string err;
    const auto header = ParseY4mHeader("YUV4MPEG2 W1 H1 F1:1 C422\n", err);
    EXPECT_FALSE(header.has_value());
    EXPECT_NE(err.find("chroma"), std::string::npos) << err;
}

TEST(ParseY4mHeader, RejectsMissingChromaTag) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader("YUV4MPEG2 W1 H1 F1:1\n", err).has_value());
}

TEST(I420FrameSize, ComputesLumaPlusTwoQuarterChromaPlanes) {
    // 4x2: Y = 8 bytes, each chroma plane = 2x1 = 2 bytes -> 8 + 2 + 2 = 12.
    EXPECT_EQ(I420FrameSize(4, 2), 12u);
    // 1920x1080: 1920*1080 + 2*(960*540) = 2073600 + 1036800 = 3110400.
    EXPECT_EQ(I420FrameSize(1920, 1080), 3110400u);
}

TEST(ReadY4mFrame, ReadsOneFrameAndAdvancesOffset) {
    // width=4 height=2 -> I420 frame size 12. Two frames back to back.
    std::string data = "FRAME\n";
    data += std::string(12, '\x11'); // frame 0 payload
    data += "FRAME\n";
    data += std::string(12, '\x22'); // frame 1 payload

    std::string err;
    const auto f0 = ReadY4mFrame(data, 0, 4, 2, err);
    ASSERT_TRUE(f0.has_value()) << err;
    EXPECT_EQ(f0->data_offset, 6u); // after "FRAME\n"
    EXPECT_EQ(f0->data_size, 12u);
    EXPECT_EQ(data[f0->data_offset], '\x11');
    EXPECT_EQ(f0->next_offset, 6u + 12u);

    const auto f1 = ReadY4mFrame(data, f0->next_offset, 4, 2, err);
    ASSERT_TRUE(f1.has_value()) << err;
    EXPECT_EQ(data[f1->data_offset], '\x22');
    EXPECT_EQ(f1->next_offset, data.size());
}

TEST(ReadY4mFrame, ReturnsNulloptWithEmptyErrorAtCleanEof) {
    const std::string data = "FRAME\n" + std::string(12, '\0');
    std::string err;
    const auto f0 = ReadY4mFrame(data, 0, 4, 2, err);
    ASSERT_TRUE(f0.has_value());
    const auto eof = ReadY4mFrame(data, f0->next_offset, 4, 2, err);
    EXPECT_FALSE(eof.has_value());
    EXPECT_TRUE(err.empty());
}

TEST(ReadY4mFrame, RejectsMalformedFrameMarker) {
    const std::string data = "WRONG\n" + std::string(12, '\0');
    std::string err;
    EXPECT_FALSE(ReadY4mFrame(data, 0, 4, 2, err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(ReadY4mFrame, RejectsTruncatedFrameData) {
    const std::string data = "FRAME\n" + std::string(5, '\0'); // needs 12 bytes, only 5 present
    std::string err;
    EXPECT_FALSE(ReadY4mFrame(data, 0, 4, 2, err).has_value());
    EXPECT_FALSE(err.empty());
}

} // namespace
} // namespace exosnap::engine

// ---------------------------------------------------------------------------
// The even-dimension precondition, enforced where the dimensions are first known
// ---------------------------------------------------------------------------
//
// I420 halves both dimensions for the chroma planes, and I420FrameSize does it
// with integer division -- correct for the documented contract that both are
// even, silently wrong for anything else. An odd width makes the computed frame
// size SMALLER than the data a real encoder writes, so the mismatch surfaces
// later as a torn frame or a rejected GPU configuration rather than as a bad
// file.
//
// Ceil-rounding the chroma planes instead would be a different pixel layout, not
// a fix for this one -- so the parser rejects, and the even cases are unchanged.

namespace {

using exosnap::engine::I420FrameSize;
using exosnap::engine::ParseY4mHeader;

std::string HeaderLine(uint32_t width, uint32_t height) {
    return "YUV4MPEG2 W" + std::to_string(width) + " H" + std::to_string(height) + " F30:1 C420\n";
}

TEST(Y4mEvenDimensions, AnOddWidthIsRejectedWithTheReason) {
    std::string err;
    const auto header = ParseY4mHeader(HeaderLine(1919, 1080), err);
    EXPECT_FALSE(header.has_value()) << "an odd width was accepted";
    EXPECT_NE(err.find("even width and height"), std::string::npos) << err;
    EXPECT_NE(err.find("1919"), std::string::npos) << "the reason must name the dimensions: " << err;
}

TEST(Y4mEvenDimensions, AnOddHeightIsRejected) {
    // 1079 rows: the concrete counter-example from the review.
    std::string err;
    EXPECT_FALSE(ParseY4mHeader(HeaderLine(1920, 1079), err).has_value());
    EXPECT_NE(err.find("1079"), std::string::npos) << err;
}

TEST(Y4mEvenDimensions, BothOddIsRejected) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader(HeaderLine(1919, 1079), err).has_value());
}

TEST(Y4mEvenDimensions, ZeroIsRejectedAsItsOwnCase) {
    // Zero is even, so the parity check alone would let it through -- and a
    // zero-sized frame is not a frame.
    std::string err;
    EXPECT_FALSE(ParseY4mHeader(HeaderLine(0, 1080), err).has_value());
    EXPECT_NE(err.find("positive"), std::string::npos) << err;
    err.clear();
    EXPECT_FALSE(ParseY4mHeader(HeaderLine(1920, 0), err).has_value());
}

TEST(Y4mEvenDimensions, EvenDimensionsAreUnchanged) {
    // The control: everything that worked before still works, including the small
    // and odd-looking-but-even sizes.
    for (const auto [w, h] :
         {std::pair<uint32_t, uint32_t>{1920, 1080}, {2, 2}, {1280, 720}, {3840, 2160}, {642, 482}}) {
        std::string err;
        const auto header = ParseY4mHeader(HeaderLine(w, h), err);
        ASSERT_TRUE(header.has_value()) << w << "x" << h << ": " << err;
        EXPECT_EQ(header->width, w);
        EXPECT_EQ(header->height, h);
    }
}

TEST(Y4mEvenDimensions, TheFrameSizeArithmeticIsOnlyCorrectForEvenDimensions) {
    // The premise, pinned: for an even pair the planes add up exactly, and for an
    // odd one the integer division loses the last row/column -- which is why the
    // parser refuses rather than this function rounding differently.
    EXPECT_EQ(I420FrameSize(4, 4), 16u + 2u * 4u);
    EXPECT_EQ(I420FrameSize(1920, 1080), 1920u * 1080u * 3u / 2u);

    // 3x3: a full 9-byte Y plane, but 2x(1x1) chroma instead of the 2x(2x2) a
    // ceil layout would use. The difference is the defect the contract avoids.
    EXPECT_EQ(I420FrameSize(3, 3), 9u + 2u * 1u);
    EXPECT_LT(I420FrameSize(3, 3), 9u + 2u * 4u);
}

} // namespace