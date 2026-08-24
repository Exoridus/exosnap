#include "../src/elementary_stream_writer.h"

#include <gtest/gtest.h>

namespace exosnap::engine {
namespace {

uint16_t ReadU16Le(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t ReadU32Le(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint64_t ReadU64Le(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | b[off + static_cast<size_t>(i)];
    return v;
}

TEST(BuildIvfFileHeader, IsExactly32BytesWithDkifSignature) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    ASSERT_EQ(h.size(), 32u);
    EXPECT_EQ(h[0], 'D');
    EXPECT_EQ(h[1], 'K');
    EXPECT_EQ(h[2], 'I');
    EXPECT_EQ(h[3], 'F');
}

TEST(BuildIvfFileHeader, EncodesVersionAndHeaderLength) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    EXPECT_EQ(ReadU16Le(h, 4), 0u);  // version
    EXPECT_EQ(ReadU16Le(h, 6), 32u); // header length
}

TEST(BuildIvfFileHeader, EncodesAv01FourCc) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    EXPECT_EQ(h[8], 'A');
    EXPECT_EQ(h[9], 'V');
    EXPECT_EQ(h[10], '0');
    EXPECT_EQ(h[11], '1');
}

TEST(BuildIvfFileHeader, EncodesDimensionsFramerateAndFrameCount) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    EXPECT_EQ(ReadU16Le(h, 12), 1920u);
    EXPECT_EQ(ReadU16Le(h, 14), 1080u);
    EXPECT_EQ(ReadU32Le(h, 16), 60u);  // rate
    EXPECT_EQ(ReadU32Le(h, 20), 1u);   // scale
    EXPECT_EQ(ReadU32Le(h, 24), 300u); // frame count
    EXPECT_EQ(ReadU32Le(h, 28), 0u);   // reserved
}

TEST(BuildIvfFrameHeader, IsExactly12BytesWithSizeThenPts) {
    const auto h = BuildIvfFrameHeader(12345, 7);
    ASSERT_EQ(h.size(), 12u);
    EXPECT_EQ(ReadU32Le(h, 0), 12345u);
    EXPECT_EQ(ReadU64Le(h, 4), 7u);
}

TEST(BuildIvfFrameHeader, HandlesLargeFrameIndex) {
    const auto h = BuildIvfFrameHeader(1, 0x1'0000'0001ull);
    EXPECT_EQ(ReadU64Le(h, 4), 0x1'0000'0001ull);
}

} // namespace
} // namespace exosnap::engine
