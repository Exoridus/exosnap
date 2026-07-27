#include "nvenc_encoder.h"

#include <gtest/gtest.h>

// Pure, GPU-free unit tests for the NVENC chroma/input-format helpers that back
// the expert 4:4:4 encode path:
//   NvencInputFormat   — bit depth + chroma -> NVENC input buffer format
//   NvencChromaFormatIDC — chroma -> NV_ENC_CONFIG chromaFormatIDC (1 / 3)
//   Nvenc444ProfileGuid — codec -> 4:4:4 profile GUID (or zero for AV1)
//
// These pin the byte-level contract without opening an NVENC session:
//   * the 4:2:0 path is unchanged (NV12 8-bit, P010 10-bit, chromaFormatIDC=1)
//   * 4:4:4 selects AYUV + chromaFormatIDC=3 + the codec's 4:4:4 profile
//   * AV1 has no 4:4:4 profile (all-zero GUID) so 4:4:4 can never be enabled.

namespace recorder_core {
namespace {

bool IsZeroGuid(const GUID& g) {
    static const GUID kZero{};
    return IsEqualGUID(g, kZero) != 0;
}

// --- 4:2:0 path is byte-identical to before ----------------------------------

TEST(NvencChromaConfig, Cs420InputFormatsUnchanged) {
    EXPECT_EQ(NvencInputFormat(BitDepth::Bit8, ChromaSubsampling::Cs420), NV_ENC_BUFFER_FORMAT_NV12);
    EXPECT_EQ(NvencInputFormat(BitDepth::Bit10, ChromaSubsampling::Cs420), NV_ENC_BUFFER_FORMAT_YUV420_10BIT);
}

TEST(NvencChromaConfig, Cs420ChromaFormatIdcIsOne) {
    EXPECT_EQ(NvencChromaFormatIDC(ChromaSubsampling::Cs420), 1u);
}

// --- 4:4:4 selects AYUV + IDC 3 ----------------------------------------------

TEST(NvencChromaConfig, Cs444InputFormatIsAyuv) {
    EXPECT_EQ(NvencInputFormat(BitDepth::Bit8, ChromaSubsampling::Cs444), NV_ENC_BUFFER_FORMAT_AYUV);
}

TEST(NvencChromaConfig, Cs444ChromaFormatIdcIsThree) {
    EXPECT_EQ(NvencChromaFormatIDC(ChromaSubsampling::Cs444), 3u);
}

// --- 4:4:4 profile GUIDs ------------------------------------------------------

TEST(NvencChromaConfig, H264Uses444Profile) {
    EXPECT_TRUE(IsEqualGUID(Nvenc444ProfileGuid(VideoCodec::H264), NV_ENC_H264_PROFILE_HIGH_444_GUID) != 0);
}

TEST(NvencChromaConfig, HevcUsesFrextProfile) {
    EXPECT_TRUE(IsEqualGUID(Nvenc444ProfileGuid(VideoCodec::Hevc), NV_ENC_HEVC_PROFILE_FREXT_GUID) != 0);
}

TEST(NvencChromaConfig, Av1HasNo444Profile) {
    EXPECT_TRUE(IsZeroGuid(Nvenc444ProfileGuid(VideoCodec::Av1)));
}

} // namespace
} // namespace recorder_core
