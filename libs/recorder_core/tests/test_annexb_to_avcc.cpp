#include <gtest/gtest.h>

#include "annexb_to_avcc.h"

#include <cstdint>
#include <vector>

namespace recorder_core::annexb {
namespace {

// Minimal fake Annex-B packet:
//   00 00 00 01 67 ... (SPS, type=7)
//   00 00 00 01 68 ... (PPS, type=8)
//   00 00 00 01 65 ... (IDR, type=5)
static std::vector<uint8_t> MakeFakeAnnexB(bool includeSps = true, bool includePps = true) {
    std::vector<uint8_t> bs;
    if (includeSps) {
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB}); // SPS (type=7)
    }
    if (includePps) {
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x68, 0xCC}); // PPS (type=8)
    }
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22, 0x33}); // IDR (type=5)
    return bs;
}

TEST(AnnexBTest, ExtractsSpsAndPpsFromValidPacket) {
    const auto bs = MakeFakeAnnexB();
    std::vector<uint8_t> out;
    EXPECT_TRUE(ExtractH264SpsAndPps(bs.data(), bs.size(), out));

    // Output must start with 4-byte start code + SPS payload
    ASSERT_GE(out.size(), 4u + 3u + 4u + 2u); // sc+SPS + sc+PPS minimum
    EXPECT_EQ(out[0], 0x00u);
    EXPECT_EQ(out[1], 0x00u);
    EXPECT_EQ(out[2], 0x00u);
    EXPECT_EQ(out[3], 0x01u);
    EXPECT_EQ(out[4], 0x67u); // SPS NAL byte
}

TEST(AnnexBTest, ExtractFailsWithoutSps) {
    const auto bs = MakeFakeAnnexB(false, true);
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractH264SpsAndPps(bs.data(), bs.size(), out));
}

TEST(AnnexBTest, ExtractFailsWithoutPps) {
    const auto bs = MakeFakeAnnexB(true, false);
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractH264SpsAndPps(bs.data(), bs.size(), out));
}

TEST(AnnexBTest, ExtractFailsOnEmptyInput) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractH264SpsAndPps(nullptr, 0, out));
}

TEST(AnnexBTest, NullPointerWithNonzeroSizeReturnsFalse) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractH264SpsAndPps(nullptr, 4, out));
}

TEST(AnnexBTest, ThreeByteStartCodeIsAlsoRecognized) {
    // 3-byte start code (00 00 01) instead of 4-byte
    std::vector<uint8_t> bs;
    bs.insert(bs.end(), {0x00, 0x00, 0x01, 0x67, 0xAA}); // SPS
    bs.insert(bs.end(), {0x00, 0x00, 0x01, 0x68, 0xBB}); // PPS
    bs.insert(bs.end(), {0x00, 0x00, 0x01, 0x65, 0xCC}); // IDR

    std::vector<uint8_t> out;
    EXPECT_TRUE(ExtractH264SpsAndPps(bs.data(), bs.size(), out));
    EXPECT_EQ(out[4], 0x67u); // SPS NAL type byte
}

} // namespace
} // namespace recorder_core::annexb
