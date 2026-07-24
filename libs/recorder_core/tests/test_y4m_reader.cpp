#include "../src/y4m_reader.h"

#include <gtest/gtest.h>

namespace recorder_core {
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
} // namespace recorder_core
