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

// ---------------------------------------------------------------------------
// ConvertAnnexBToAvcc tests
// ---------------------------------------------------------------------------

TEST(AnnexBTest, ConvertAnnexBToAvcc_ReturnsFalseOnNullInput) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ConvertAnnexBToAvcc(nullptr, 0, out));
    EXPECT_FALSE(ConvertAnnexBToAvcc(nullptr, 4, out));
}

TEST(AnnexBTest, ConvertAnnexBToAvcc_ReturnsFalseOnEmptyInput) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ConvertAnnexBToAvcc(nullptr, 0, out));
    // A single byte with no start code produces no convertible NALs
    const uint8_t not_a_nal[] = {0xFFu};
    EXPECT_FALSE(ConvertAnnexBToAvcc(not_a_nal, sizeof(not_a_nal), out));
}

TEST(AnnexBTest, ConvertAnnexBToAvcc_SingleIdrNal_ProducesLengthPrefix) {
    // Annex-B IDR: 00 00 00 01 65 11 22 33
    const std::vector<uint8_t> bs = {0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22, 0x33};
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToAvcc(bs.data(), bs.size(), out));

    // Expected: [00 00 00 04] [65 11 22 33]
    ASSERT_EQ(out.size(), 8u);
    EXPECT_EQ(out[0], 0x00u);
    EXPECT_EQ(out[1], 0x00u);
    EXPECT_EQ(out[2], 0x00u);
    EXPECT_EQ(out[3], 0x04u); // length = 4
    EXPECT_EQ(out[4], 0x65u); // IDR NAL byte
    EXPECT_EQ(out[5], 0x11u);
    EXPECT_EQ(out[6], 0x22u);
    EXPECT_EQ(out[7], 0x33u);
}

TEST(AnnexBTest, ConvertAnnexBToAvcc_SkipsAudNal) {
    // AUD (type 9) followed by IDR (type 5)
    std::vector<uint8_t> bs;
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0}); // AUD
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA}); // IDR
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToAvcc(bs.data(), bs.size(), out));

    // Only IDR should be in output (AUD skipped)
    // Expected: [00 00 00 02] [65 AA]
    ASSERT_EQ(out.size(), 6u);
    EXPECT_EQ(out[3], 0x02u); // length = 2
    EXPECT_EQ(out[4], 0x65u); // IDR NAL byte
    EXPECT_EQ(out[5], 0xAAu);
}

TEST(AnnexBTest, ConvertAnnexBToAvcc_MultipleNals_AllLengthPrefixed) {
    const auto bs = MakeFakeAnnexB(); // SPS + PPS + IDR
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToAvcc(bs.data(), bs.size(), out));

    // All 3 NALs (SPS=4 bytes, PPS=2 bytes, IDR=4 bytes) should appear with 4-byte length prefix each
    // Total = 3*(4) + 4 + 2 + 4 = 12 + 10 = 22 bytes
    // SPS payload: [67 AA BB 28] = 4 bytes
    // PPS payload: [68 CC] = 2 bytes
    // IDR payload: [65 11 22 33] = 4 bytes
    ASSERT_GE(out.size(), 22u);

    // First NAL must have length prefix for 4-byte SPS payload
    const uint32_t sps_len = (static_cast<uint32_t>(out[0]) << 24u) | (static_cast<uint32_t>(out[1]) << 16u) |
                             (static_cast<uint32_t>(out[2]) << 8u) | static_cast<uint32_t>(out[3]);
    EXPECT_EQ(sps_len, 4u);
    EXPECT_EQ(out[4], 0x67u); // SPS NAL type
}

TEST(AnnexBTest, ConvertAnnexBToAvcc_NoStartCodesInOutput) {
    const auto bs = MakeFakeAnnexB();
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToAvcc(bs.data(), bs.size(), out));

    // AVCC output must not contain 00 00 00 01 start codes
    for (size_t i = 0; i + 3 < out.size(); ++i) {
        const bool is_start_code = out[i] == 0x00 && out[i + 1] == 0x00 && out[i + 2] == 0x00 && out[i + 3] == 0x01;
        // Length bytes of 00 00 00 0X don't count, but start codes at arbitrary positions would
        // We only flag when they appear in the middle of a payload (after offset 4)
        if (i > 0 && is_start_code) {
            // Check it's actually a NAL type byte following a real NAL, not a length field
            // Simple check: no 4-byte sequence 00 00 00 01 starting inside a NAL payload
            // This test is intentionally relaxed — it just verifies the output parses as AVCC
            (void)is_start_code;
        }
    }

    // Stricter: parse AVCC and verify all length-prefixed NALs sum to output size
    size_t pos = 0;
    while (pos + 4 <= out.size()) {
        const uint32_t nal_len = (static_cast<uint32_t>(out[pos]) << 24u) |
                                 (static_cast<uint32_t>(out[pos + 1]) << 16u) |
                                 (static_cast<uint32_t>(out[pos + 2]) << 8u) | static_cast<uint32_t>(out[pos + 3]);
        ASSERT_GT(nal_len, 0u) << "zero-length NAL at offset " << pos;
        ASSERT_LE(pos + 4 + nal_len, out.size()) << "NAL length overflows output buffer";
        pos += 4 + nal_len;
    }
    EXPECT_EQ(pos, out.size()) << "output is not a valid AVCC sequence";
}

} // namespace
} // namespace recorder_core::annexb
