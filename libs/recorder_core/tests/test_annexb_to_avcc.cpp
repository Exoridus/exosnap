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
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0xBB, 0x28}); // SPS (type=7, level=0x28)
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
    ASSERT_GE(out.size(), 4u + 4u + 4u + 2u); // sc+SPS + sc+PPS minimum
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

// ---------------------------------------------------------------------------
// BuildAvccFromAnnexBSpsAndPps tests
// ---------------------------------------------------------------------------

TEST(AnnexBTest, BuildAvcc_ProducesCorrectHeaderFromValidSpsAndPps) {
    const auto bs = MakeFakeAnnexB();
    std::vector<uint8_t> sps_pps;
    ASSERT_TRUE(ExtractH264SpsAndPps(bs.data(), bs.size(), sps_pps));

    std::vector<uint8_t> avcc;
    EXPECT_TRUE(BuildAvccFromAnnexBSpsAndPps(sps_pps, avcc));

    // AVCC header: configurationVersion(1)=0x01, profile(1), compat(1), level(1), 0xFF, 0xE1, spsLen(2), sps, 0x01,
    // ppsLen(2), pps
    ASSERT_GE(avcc.size(), 7u);
    EXPECT_EQ(avcc[0], 0x01u); // configurationVersion
    EXPECT_EQ(avcc[4], 0xFFu); // lengthSizeMinusOne
    EXPECT_EQ(avcc[5], 0xE1u); // numSPS = 1 with reserved bits
}

TEST(AnnexBTest, BuildAvcc_FailsOnEmptyInput) {
    std::vector<uint8_t> empty;
    std::vector<uint8_t> avcc;
    EXPECT_FALSE(BuildAvccFromAnnexBSpsAndPps(empty, avcc));
}

TEST(AnnexBTest, BuildAvcc_FailsOnInputWithNoSecondStartCode) {
    // Only SPS, no PPS start code
    std::vector<uint8_t> only_sps = {0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1F,
                                     0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<uint8_t> avcc;
    EXPECT_FALSE(BuildAvccFromAnnexBSpsAndPps(only_sps, avcc));
}

TEST(AnnexBTest, BuildAvcc_SpsProfileBytesMatchAvccHeader) {
    // Craft a specific SPS: profile=0x64, compat=0x00, level=0x28
    // SPS NAL: 0x67 0x64 0x00 0x28 [extra bytes]
    std::vector<uint8_t> sps_pps;
    sps_pps.insert(sps_pps.end(), {0x00, 0x00, 0x00, 0x01});             // start code
    sps_pps.insert(sps_pps.end(), {0x67, 0x64, 0x00, 0x28, 0xAD, 0xCC}); // SPS
    sps_pps.insert(sps_pps.end(), {0x00, 0x00, 0x00, 0x01});             // start code
    sps_pps.insert(sps_pps.end(), {0x68, 0xCE, 0x38, 0x80});             // PPS

    std::vector<uint8_t> avcc;
    ASSERT_TRUE(BuildAvccFromAnnexBSpsAndPps(sps_pps, avcc));

    EXPECT_EQ(avcc[0], 0x01u); // configurationVersion
    EXPECT_EQ(avcc[1], 0x64u); // profile_idc
    EXPECT_EQ(avcc[2], 0x00u); // profile_compatibility
    EXPECT_EQ(avcc[3], 0x28u); // level_idc
}

} // namespace
} // namespace recorder_core::annexb
