#include <gtest/gtest.h>

#include "annexb_to_hvcc.h"

#include <cstdint>
#include <vector>

namespace recorder_core::annexb {
namespace {

// ---------------------------------------------------------------------------
// Minimal synthetic Annex-B HEVC packet helpers
// ---------------------------------------------------------------------------
//
// HEVC NAL header: 2 bytes.
//   Byte 0: forbidden(1b=0) + nal_unit_type(6b) + nuh_layer_id_msb(1b=0)
//   Byte 1: nuh_layer_id_lsb(5b=0) + nuh_temporal_id_plus1(3b=1)
// So:
//   VPS (type=32): header = [(32<<1)|0, 0x01] = [0x40, 0x01]
//   SPS (type=33): header = [(33<<1)|0, 0x01] = [0x42, 0x01]
//   PPS (type=34): header = [(34<<1)|0, 0x01] = [0x44, 0x01]
//   AUD (type=35): header = [(35<<1)|0, 0x01] = [0x46, 0x01]

// Minimal MSB-first bit writer for synthesizing a real HEVC SPS RBSP.
class SpsBitWriter {
  public:
    void PutBit(uint32_t bit) {
        cur_ = static_cast<uint8_t>((cur_ << 1u) | (bit & 0x1u));
        if (++nbits_ == 8u) {
            bytes_.push_back(cur_);
            cur_ = 0;
            nbits_ = 0;
        }
    }
    void PutBits(uint32_t value, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i)
            PutBit((value >> (count - 1u - i)) & 0x1u);
    }
    // Exp-Golomb ue(v): n leading zeros, a 1, then n suffix bits.
    void PutUe(uint32_t value) {
        const uint32_t code = value + 1u;
        uint32_t n = 0;
        while ((1u << (n + 1u)) <= code)
            ++n;
        for (uint32_t i = 0; i < n; ++i)
            PutBit(0u);
        for (int32_t i = static_cast<int32_t>(n); i >= 0; --i)
            PutBit((code >> i) & 0x1u);
    }
    std::vector<uint8_t> Finish() {
        // rbsp_stop_one_bit then byte alignment.
        PutBit(1u);
        while (nbits_ != 0u)
            PutBit(0u);
        return bytes_;
    }

  private:
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0;
    uint32_t nbits_ = 0;
};

// Build a real, parseable HEVC SPS NAL (2-byte header + RBSP) for the given
// profile_idc / bit depth. profile_idc=1 (Main, 8-bit) or 2 (Main10, 10-bit).
// level_idc is written as 0x5D and chroma_format_idc as 1 (4:2:0).
static std::vector<uint8_t> MakeHevcSpsNal(uint8_t profile_idc, uint32_t bit_depth_minus8) {
    SpsBitWriter w;
    w.PutBits(0u, 4u); // sps_video_parameter_set_id
    w.PutBits(0u, 3u); // sps_max_sub_layers_minus1
    w.PutBit(1u);      // sps_temporal_id_nesting_flag

    // profile_tier_level (general layer, 12 bytes)
    w.PutBits(0u, 2u);                 // general_profile_space
    w.PutBit(0u);                      // general_tier_flag
    w.PutBits(profile_idc, 5u);        // general_profile_idc
    w.PutBits(0x60000000u >> 0u, 32u); // general_profile_compatibility_flags (Main bit set)
    w.PutBits(0x90u, 8u);              // constraint byte 0 (progressive+frame_only)
    for (int i = 0; i < 5; ++i)        // constraint bytes 1-5
        w.PutBits(0u, 8u);
    w.PutBits(0x5Du, 8u); // general_level_idc

    w.PutUe(0u);               // sps_seq_parameter_set_id
    w.PutUe(1u);               // chroma_format_idc = 1 (4:2:0)
    w.PutUe(1920u);            // pic_width_in_luma_samples
    w.PutUe(1080u);            // pic_height_in_luma_samples
    w.PutBit(0u);              // conformance_window_flag
    w.PutUe(bit_depth_minus8); // bit_depth_luma_minus8
    w.PutUe(bit_depth_minus8); // bit_depth_chroma_minus8
    // (Remaining SPS syntax omitted; the parser stops after bit_depth_chroma_minus8.)

    std::vector<uint8_t> rbsp = w.Finish();
    std::vector<uint8_t> nal = {0x42, 0x01}; // SPS NAL header (type 33)
    nal.insert(nal.end(), rbsp.begin(), rbsp.end());
    return nal;
}

// Builds a minimal Annex-B HEVC packet with the requested NAL types.
// VPS/PPS carry short synthetic payloads; the SPS is a real, parseable Main (8-bit,
// 4:2:0) SPS so BuildHvcc reflects real profile/level/chroma/bit-depth values.
static std::vector<uint8_t> MakeFakeHevcAnnexB(bool includeVps = true, bool includeSps = true, bool includePps = true,
                                               bool includeAud = false, bool includeIdr = true) {
    std::vector<uint8_t> bs;

    if (includeAud) {
        // AUD (type 35): header + 1 byte payload
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01}); // start code
        bs.insert(bs.end(), {0x46, 0x01, 0x50});       // AUD NAL header + payload
    }

    if (includeVps) {
        // VPS (type 32): header + short payload
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01});       // start code
        bs.insert(bs.end(), {0x40, 0x01, 0xAA, 0xBB, 0xCC}); // VPS NAL header + payload
    }

    if (includeSps) {
        // SPS (type 33): a real Main / 8-bit / 4:2:0 SPS with general_level_idc=0x5D.
        const auto sps = MakeHevcSpsNal(/*profile_idc=*/1u, /*bit_depth_minus8=*/0u);
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01}); // start code
        bs.insert(bs.end(), sps.begin(), sps.end());
    }

    if (includePps) {
        // PPS (type 34): header + short payload
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01}); // start code
        bs.insert(bs.end(), {0x44, 0x01, 0xDD, 0xEE}); // PPS NAL header + payload
    }

    if (includeIdr) {
        // IDR frame (type 19): not a VPS/SPS/PPS, just data
        // IDR type 19: (19<<1)|0 = 0x26
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01});       // start code
        bs.insert(bs.end(), {0x26, 0x01, 0x11, 0x22, 0x33}); // IDR NAL
    }

    return bs;
}

// ---------------------------------------------------------------------------
// ExtractHevcVpsSpsPps tests
// ---------------------------------------------------------------------------

TEST(AnnexBHvccTest, ExtractSucceedsWithVpsSpsPps) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> out;
    EXPECT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));

    // Output must start with 4-byte start code
    ASSERT_GE(out.size(), 4u);
    EXPECT_EQ(out[0], 0x00u);
    EXPECT_EQ(out[1], 0x00u);
    EXPECT_EQ(out[2], 0x00u);
    EXPECT_EQ(out[3], 0x01u);

    // First NAL must be VPS (type 32: byte0 = 0x40)
    EXPECT_EQ(out[4], 0x40u);
}

TEST(AnnexBHvccTest, ExtractFailsWithoutVps) {
    const auto bs = MakeFakeHevcAnnexB(/*vps=*/false, /*sps=*/true, /*pps=*/true);
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));
}

TEST(AnnexBHvccTest, ExtractFailsWithoutSps) {
    const auto bs = MakeFakeHevcAnnexB(/*vps=*/true, /*sps=*/false, /*pps=*/true);
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));
}

TEST(AnnexBHvccTest, ExtractFailsWithoutPps) {
    const auto bs = MakeFakeHevcAnnexB(/*vps=*/true, /*sps=*/true, /*pps=*/false);
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));
}

TEST(AnnexBHvccTest, ExtractFailsOnNullInput) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ExtractHevcVpsSpsPps(nullptr, 0, out));
    EXPECT_FALSE(ExtractHevcVpsSpsPps(nullptr, 4, out));
}

TEST(AnnexBHvccTest, ExtractOutputContainsThreeStartCodeSeparatedNals) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> out;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));

    // Count 4-byte start codes in output
    int sc_count = 0;
    for (size_t i = 0; i + 3 < out.size(); ++i) {
        if (out[i] == 0x00 && out[i + 1] == 0x00 && out[i + 2] == 0x00 && out[i + 3] == 0x01)
            ++sc_count;
    }
    EXPECT_EQ(sc_count, 3); // VPS, SPS, PPS — each preceded by one start code
}

TEST(AnnexBHvccTest, ExtractPreservesVpsSpsPpsNalTypes) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> out;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));

    // Collect all start codes and check the first byte of each NAL payload
    std::vector<uint8_t> nal_first_bytes;
    for (size_t i = 0; i + 4 < out.size(); ++i) {
        if (out[i] == 0x00 && out[i + 1] == 0x00 && out[i + 2] == 0x00 && out[i + 3] == 0x01) {
            if (i + 4 < out.size())
                nal_first_bytes.push_back(out[i + 4]);
            i += 3; // skip past start code
        }
    }
    ASSERT_EQ(nal_first_bytes.size(), 3u);
    // VPS header byte0 = 0x40 (type=32), SPS = 0x42 (type=33), PPS = 0x44 (type=34)
    EXPECT_EQ(nal_first_bytes[0], 0x40u); // VPS
    EXPECT_EQ(nal_first_bytes[1], 0x42u); // SPS
    EXPECT_EQ(nal_first_bytes[2], 0x44u); // PPS
}

TEST(AnnexBHvccTest, ExtractWorksWhenAudPrecedesVps) {
    // AUD before VPS — AUD must not prevent extraction
    const auto bs = MakeFakeHevcAnnexB(/*vps=*/true, /*sps=*/true, /*pps=*/true, /*aud=*/true);
    std::vector<uint8_t> out;
    EXPECT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));
    // First NAL in output must be VPS, not AUD
    ASSERT_GE(out.size(), 5u);
    EXPECT_EQ(out[4], 0x40u); // VPS first byte
}

// ---------------------------------------------------------------------------
// BuildHvccFromAnnexBVpsSpsPps tests
// ---------------------------------------------------------------------------

TEST(AnnexBHvccTest, BuildHvcc_FailsOnEmptyInput) {
    std::vector<uint8_t> empty;
    std::vector<uint8_t> hvcc;
    EXPECT_FALSE(BuildHvccFromAnnexBVpsSpsPps(empty, hvcc));
}

TEST(AnnexBHvccTest, BuildHvcc_FailsOnInputTooShort) {
    // Less than 18 bytes minimum
    std::vector<uint8_t> short_input = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01};
    std::vector<uint8_t> hvcc;
    EXPECT_FALSE(BuildHvccFromAnnexBVpsSpsPps(short_input, hvcc));
}

TEST(AnnexBHvccTest, BuildHvcc_FailsOnMissingStartCode) {
    // Valid size but no 4-byte start code at offset 0
    std::vector<uint8_t> bad(20, 0xFFu);
    std::vector<uint8_t> hvcc;
    EXPECT_FALSE(BuildHvccFromAnnexBVpsSpsPps(bad, hvcc));
}

TEST(AnnexBHvccTest, BuildHvcc_ProducesNonEmptyOutput) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));
    EXPECT_GT(hvcc.size(), 23u); // At least fixed header + minimal NAL arrays
}

TEST(AnnexBHvccTest, BuildHvcc_ConfigurationVersionIsOne) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    EXPECT_EQ(hvcc[0], 0x01u); // configurationVersion = 1
}

TEST(AnnexBHvccTest, BuildHvcc_GeneralProfileByteIsMainProfile) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[1]: general_profile_space(2b=0) + general_tier_flag(1b=0) + general_profile_idc(5b=1)
    // = 0b00_0_00001 = 0x01
    EXPECT_EQ(hvcc[1], 0x01u);
}

TEST(AnnexBHvccTest, BuildHvcc_ProfileCompatibilityFlagsMatchMainProfile) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[2-5]: general_profile_compatibility_flags = 0x60000000 (Main + MainStillPicture)
    EXPECT_EQ(hvcc[2], 0x60u);
    EXPECT_EQ(hvcc[3], 0x00u);
    EXPECT_EQ(hvcc[4], 0x00u);
    EXPECT_EQ(hvcc[5], 0x00u);
}

TEST(AnnexBHvccTest, BuildHvcc_LengthSizeMinusOneIsThree) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[21]: constantFrameRate(2b=0)+numTemporalLayers(3b=1)+temporalIdNested(1b=1)+lengthSizeMinusOne(2b=3)
    // = 0b00_001_1_11 = 0x0F
    EXPECT_EQ(hvcc[21], 0x0Fu);
}

TEST(AnnexBHvccTest, BuildHvcc_NumArraysIsThree) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[22]: numOfArrays = 3
    EXPECT_EQ(hvcc[22], 0x03u);
}

TEST(AnnexBHvccTest, BuildHvcc_FirstArrayTypeIsVps) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[23]: array_completeness(1b=1) + reserved(1b=0) + nal_unit_type(6b=32)
    // = 0b1_0_100000 = 0xA0
    EXPECT_EQ(hvcc[23], 0xA0u);
}

TEST(AnnexBHvccTest, BuildHvcc_VpsArrayHasOneNal) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[24-25]: numNalus for VPS array (big-endian) = 0x0001
    EXPECT_EQ(hvcc[24], 0x00u);
    EXPECT_EQ(hvcc[25], 0x01u);
}

TEST(AnnexBHvccTest, BuildHvcc_GeneralLevelIdcExtractedFromSps) {
    // Our synthetic SPS has general_level_idc = 0x5D at the right offset
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[12]: general_level_idc (after 12 bytes of fixed fields)
    // Fixed layout:
    //   [0]:   configurationVersion
    //   [1]:   general_profile_space/tier/profile_idc
    //   [2-5]: general_profile_compatibility_flags
    //   [6-11]: general_constraint_indicator_flags (6 bytes)
    //   [12]:  general_level_idc
    EXPECT_EQ(hvcc[12], 0x5Du); // our synthetic level_idc
}

TEST(AnnexBHvccTest, BuildHvcc_SpsArrayTypeByteIsCorrect) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // Locate SPS array: after VPS array entry.
    // VPS entry: 1 (type) + 2 (numNalus) + 2 (len) + vps_payload_len bytes
    // VPS payload = [0x40, 0x01, 0xAA, 0xBB, 0xCC] = 5 bytes
    // VPS entry total bytes after hvcc[23]: 2+2+5 = 9 bytes; SPS starts at hvcc[23+1+9] = hvcc[33]
    // But we calculate it dynamically from the length field.
    const size_t vps_nal_len = (static_cast<size_t>(hvcc[26]) << 8u) | static_cast<size_t>(hvcc[27]);
    const size_t sps_array_offset = 23u + 1u + 2u + 2u + vps_nal_len;
    ASSERT_LT(sps_array_offset, hvcc.size());

    // SPS array type byte: 0b1_0_100001 = 0xA1
    EXPECT_EQ(hvcc[sps_array_offset], 0xA1u);
}

TEST(AnnexBHvccTest, BuildHvcc_PpsArrayTypeByteIsCorrect) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // Dynamically find PPS array offset
    // VPS array starts at hvcc[23]
    const size_t vps_nal_len = (static_cast<size_t>(hvcc[26]) << 8u) | static_cast<size_t>(hvcc[27]);
    const size_t sps_array_offset = 23u + 1u + 2u + 2u + vps_nal_len;
    ASSERT_LT(sps_array_offset + 4u, hvcc.size());
    const size_t sps_nal_len =
        (static_cast<size_t>(hvcc[sps_array_offset + 3u]) << 8u) | static_cast<size_t>(hvcc[sps_array_offset + 4u]);
    const size_t pps_array_offset = sps_array_offset + 1u + 2u + 2u + sps_nal_len;
    ASSERT_LT(pps_array_offset, hvcc.size());

    // PPS array type byte: 0b1_0_100010 = 0xA2
    EXPECT_EQ(hvcc[pps_array_offset], 0xA2u);
}

TEST(AnnexBHvccTest, BuildHvcc_ChromaFormatByteIs4_2_0) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // Fixed header byte layout (from BuildHvccFromAnnexBVpsSpsPps):
    //   [0]:   configurationVersion
    //   [1]:   general_profile_space/tier/profile_idc
    //   [2-5]: general_profile_compatibility_flags
    //   [6-11]: general_constraint_indicator_flags (6 bytes)
    //   [12]:  general_level_idc
    //   [13-14]: min_spatial_segmentation_idc
    //   [15]:  parallelismType
    //   [16]:  chroma_format_idc = 0xFD (reserved=0xFC, chromaFormat=1 for 4:2:0)
    //   [17]:  bit_depth_luma_minus8
    //   [18]:  bit_depth_chroma_minus8
    //   [19-20]: avgFrameRate
    //   [21]:  misc byte (lengthSizeMinusOne etc.)
    //   [22]:  numOfArrays
    EXPECT_EQ(hvcc[16], 0xFDu);
}

TEST(AnnexBHvccTest, BuildHvcc_BitDepthBytesAreEightBit) {
    const auto bs = MakeFakeHevcAnnexB();
    std::vector<uint8_t> vps_sps_pps;
    ASSERT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), vps_sps_pps));

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[17]: bit_depth_luma_minus8 = 0xF8 (8-bit)
    // hvcc[18]: bit_depth_chroma_minus8 = 0xF8 (8-bit)
    EXPECT_EQ(hvcc[17], 0xF8u);
    EXPECT_EQ(hvcc[18], 0xF8u);
}

// ---------------------------------------------------------------------------
// 10-bit (Main10) hvcC tests — S5 / 0.7.0
// ---------------------------------------------------------------------------
//
// For a real Main10 (profile_idc=2, bit_depth_luma/chroma_minus8=2) SPS, the hvcC
// fixed header must reflect those values instead of the historic hardcoded 8-bit
// Main constants. These tests build a real 10-bit SPS and assert the parsed bytes.

// Wrap a single SPS NAL in start-code-prefixed VPS+SPS+PPS the way BuildHvcc expects.
static std::vector<uint8_t> WrapVpsSpsPps(const std::vector<uint8_t>& sps_nal) {
    std::vector<uint8_t> bs;
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01});
    bs.insert(bs.end(), {0x40, 0x01, 0xAA, 0xBB, 0xCC}); // VPS
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01});
    bs.insert(bs.end(), sps_nal.begin(), sps_nal.end()); // SPS
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01});
    bs.insert(bs.end(), {0x44, 0x01, 0xDD, 0xEE}); // PPS
    std::vector<uint8_t> out;
    EXPECT_TRUE(ExtractHevcVpsSpsPps(bs.data(), bs.size(), out));
    return out;
}

TEST(AnnexBHvccTest, BuildHvcc_Main10_ProfileIdcIsTwo) {
    const auto sps = MakeHevcSpsNal(/*profile_idc=*/2u, /*bit_depth_minus8=*/2u);
    const auto vps_sps_pps = WrapVpsSpsPps(sps);

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // hvcc[1]: general_profile_space(2b=0) + general_tier_flag(1b=0) + general_profile_idc(5b=2)
    // = 0b00_0_00010 = 0x02
    EXPECT_EQ(hvcc[1], 0x02u);
}

TEST(AnnexBHvccTest, BuildHvcc_Main10_BitDepthBytesAreTenBit) {
    const auto sps = MakeHevcSpsNal(/*profile_idc=*/2u, /*bit_depth_minus8=*/2u);
    const auto vps_sps_pps = WrapVpsSpsPps(sps);

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // bit_depth_luma_minus8: reserved(5b=0x1F) + value(3b=2) = 0xFA → 10-bit
    EXPECT_EQ(hvcc[17], 0xFAu);
    EXPECT_EQ(hvcc[18], 0xFAu);
}

TEST(AnnexBHvccTest, BuildHvcc_Main10_ChromaFormatStill4_2_0) {
    const auto sps = MakeHevcSpsNal(/*profile_idc=*/2u, /*bit_depth_minus8=*/2u);
    const auto vps_sps_pps = WrapVpsSpsPps(sps);

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // chroma_format_idc: reserved(6b) + chromaFormat(2b=1 for 4:2:0) = 0xFD
    EXPECT_EQ(hvcc[16], 0xFDu);
}

TEST(AnnexBHvccTest, BuildHvcc_Main10_LevelIdcExtracted) {
    const auto sps = MakeHevcSpsNal(/*profile_idc=*/2u, /*bit_depth_minus8=*/2u);
    const auto vps_sps_pps = WrapVpsSpsPps(sps);

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    // general_level_idc at hvcc[12] (we wrote 0x5D into the SPS).
    EXPECT_EQ(hvcc[12], 0x5Du);
}

TEST(AnnexBHvccTest, BuildHvcc_Main8_StillReportsEightBitProfile1) {
    // Regression guard: the real 8-bit Main SPS still yields profile_idc=1, 8-bit.
    const auto sps = MakeHevcSpsNal(/*profile_idc=*/1u, /*bit_depth_minus8=*/0u);
    const auto vps_sps_pps = WrapVpsSpsPps(sps);

    std::vector<uint8_t> hvcc;
    ASSERT_TRUE(BuildHvccFromAnnexBVpsSpsPps(vps_sps_pps, hvcc));

    EXPECT_EQ(hvcc[1], 0x01u);  // profile_idc = 1 (Main)
    EXPECT_EQ(hvcc[16], 0xFDu); // chroma 4:2:0
    EXPECT_EQ(hvcc[17], 0xF8u); // bit_depth_luma_minus8 = 0 (8-bit)
    EXPECT_EQ(hvcc[18], 0xF8u); // bit_depth_chroma_minus8 = 0 (8-bit)
}

// ---------------------------------------------------------------------------
// ConvertAnnexBToHevcSample tests
// ---------------------------------------------------------------------------

TEST(AnnexBHvccTest, ConvertSample_ReturnsFalseOnNullInput) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ConvertAnnexBToHevcSample(nullptr, 0, out));
    EXPECT_FALSE(ConvertAnnexBToHevcSample(nullptr, 4, out));
}

TEST(AnnexBHvccTest, ConvertSample_ReturnsFalseOnEmptyInput) {
    std::vector<uint8_t> out;
    // Pass a valid pointer but size=0 — no NALs to produce
    const uint8_t dummy = 0x00u;
    EXPECT_FALSE(ConvertAnnexBToHevcSample(&dummy, 0, out));
}

TEST(AnnexBHvccTest, ConvertSample_SkipsAudNal) {
    // AUD (type 35) followed by IDR (type 19)
    std::vector<uint8_t> bs;
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50}); // AUD
    bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAA}); // IDR
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToHevcSample(bs.data(), bs.size(), out));

    // Only IDR should be in output (AUD skipped)
    // IDR payload: [0x26, 0x01, 0xAA] = 3 bytes → length prefix [00 00 00 03] + payload
    ASSERT_EQ(out.size(), 7u);
    EXPECT_EQ(out[0], 0x00u);
    EXPECT_EQ(out[1], 0x00u);
    EXPECT_EQ(out[2], 0x00u);
    EXPECT_EQ(out[3], 0x03u); // length = 3
    EXPECT_EQ(out[4], 0x26u); // IDR first byte
}

TEST(AnnexBHvccTest, ConvertSample_SingleIdrNalHasFourByteLengthPrefix) {
    // IDR (type 19): [0x26, 0x01, 0x11, 0x22, 0x33] = 5 bytes
    const std::vector<uint8_t> bs = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x11, 0x22, 0x33};
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToHevcSample(bs.data(), bs.size(), out));

    // Expected: [00 00 00 05] [26 01 11 22 33]
    ASSERT_EQ(out.size(), 9u);
    EXPECT_EQ(out[0], 0x00u);
    EXPECT_EQ(out[1], 0x00u);
    EXPECT_EQ(out[2], 0x00u);
    EXPECT_EQ(out[3], 0x05u); // length = 5
    EXPECT_EQ(out[4], 0x26u); // IDR first byte
}

TEST(AnnexBHvccTest, ConvertSample_MultipleNalsAllLengthPrefixed) {
    // VPS + SPS + IDR (no PPS, no AUD) — all should appear in output
    const auto bs = MakeFakeHevcAnnexB(/*vps=*/true, /*sps=*/true, /*pps=*/false, /*aud=*/false, /*idr=*/true);
    std::vector<uint8_t> out;
    ASSERT_TRUE(ConvertAnnexBToHevcSample(bs.data(), bs.size(), out));

    // Parse output as length-prefixed sequence; verify it's consistent
    size_t pos = 0;
    int nal_count = 0;
    while (pos + 4 <= out.size()) {
        const uint32_t nal_len = (static_cast<uint32_t>(out[pos]) << 24u) |
                                 (static_cast<uint32_t>(out[pos + 1]) << 16u) |
                                 (static_cast<uint32_t>(out[pos + 2]) << 8u) | static_cast<uint32_t>(out[pos + 3]);
        ASSERT_GT(nal_len, 0u) << "zero-length NAL at offset " << pos;
        ASSERT_LE(pos + 4u + nal_len, out.size()) << "NAL length overflows output buffer";
        pos += 4u + nal_len;
        ++nal_count;
    }
    EXPECT_EQ(pos, out.size()) << "output is not a valid length-prefixed sequence";
    EXPECT_EQ(nal_count, 3); // VPS + SPS + IDR
}

TEST(AnnexBHvccTest, ConvertSample_ReturnsFalseWhenOnlyAudPresent) {
    // Only AUD — ConvertAnnexBToHevcSample skips AUDs, so output will be empty
    const auto bs = MakeFakeHevcAnnexB(/*vps=*/false, /*sps=*/false, /*pps=*/false, /*aud=*/true, /*idr=*/false);
    std::vector<uint8_t> out;
    EXPECT_FALSE(ConvertAnnexBToHevcSample(bs.data(), bs.size(), out));
}

} // namespace
} // namespace recorder_core::annexb
