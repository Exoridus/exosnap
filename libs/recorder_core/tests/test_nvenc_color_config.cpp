#include "nvenc_encoder.h"

#include <gtest/gtest.h>

// Tests for ApplyColorMetadataToNvenc — the pure, GPU-free mapping from
// ColorMetadata to the NVENC bitstream-level color signaling fields.
//
// Bug context: NVENC's H.264/HEVC VUI parameters and AV1 color_config fields
// were never populated, so the encoded bitstream carried no color
// description. Verified empirically (ffprobe against a real AV1 recording,
// and a live NVENC encode on this machine) that for AV1 specifically,
// ffmpeg/most decoders derive color_range/matrix/primaries/transfer from the
// BITSTREAM exclusively — a correctly tagged Matroska Colour element does NOT
// help; an untagged AV1 bitstream reproduces color_range=tv (studio) plus
// unknown matrix/primaries/transfer regardless of the container tag. For
// H.264 the container tag is honored as a fallback when the bitstream is
// untagged, but signaling at the bitstream level is still the correct fix
// (matches what the task requires and is authoritative for every consumer).
//
// NVENC SDK fields under test:
//   AV1:        NV_ENC_CONFIG_AV1::{colorPrimaries, transferCharacteristics,
//                                    matrixCoefficients, colorRange}
//               colorRange convention: 0 = studio swing, 1 = full swing.
//   H.264/HEVC: NV_ENC_CONFIG_H264_VUI_PARAMETERS (HEVC reuses the same
//               struct) ::{videoSignalTypePresentFlag, videoFormat,
//                          videoFullRangeFlag, colourDescriptionPresentFlag,
//                          colourPrimaries, transferCharacteristics,
//                          colourMatrix}
//               videoFullRangeFlag convention (ITU-T Annex E): 0 = limited,
//               1 = full.

namespace recorder_core {
namespace {

ColorMetadata MakeSdrColor(ColorRange range) {
    ColorMetadata c = ColorMetadata::Sdr709();
    c.range = range;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// Default ColorMetadata (fix/color-range-signaling: default flipped from Full
// to Limited — common consumer players, verified with VLC, ignore the range
// flag entirely and always expand limited->full, so Full-range recordings
// looked permanently crushed/dark regardless of correct tagging). Every
// codec's DEFAULT-constructed ColorMetadata must signal studio/limited range.
// ---------------------------------------------------------------------------

TEST(ApplyColorMetadataToNvenc, DefaultColorMetadata_IsLimitedRange) {
    EXPECT_EQ(ColorMetadata::Sdr709().range, ColorRange::Limited);
}

TEST(ApplyColorMetadataToNvenc, Av1_DefaultColorMetadata_SignalsStudioRange) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Av1, ColorMetadata::Sdr709());
    const auto& av1 = cfg.encodeCodecConfig.av1Config;
    EXPECT_EQ(av1.colorPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(av1.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(av1.matrixCoefficients, NV_ENC_VUI_MATRIX_COEFFS_BT709);
    EXPECT_EQ(av1.colorRange, 0u) << "default ColorMetadata must signal studio (0), not full (1)";
}

TEST(ApplyColorMetadataToNvenc, H264_DefaultColorMetadata_SignalsLimitedRange) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::H264, ColorMetadata::Sdr709());
    const auto& vui = cfg.encodeCodecConfig.h264Config.h264VUIParameters;
    EXPECT_EQ(vui.videoFullRangeFlag, 0u) << "default ColorMetadata must signal limited (0), not full (1)";
}

TEST(ApplyColorMetadataToNvenc, Hevc_DefaultColorMetadata_SignalsLimitedRange) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Hevc, ColorMetadata::Sdr709());
    const auto& vui = cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters;
    EXPECT_EQ(vui.videoFullRangeFlag, 0u) << "default ColorMetadata must signal limited (0), not full (1)";
}

// ---------------------------------------------------------------------------
// AV1 — colorRange convention (0=studio/1=full), BT.709 primaries/transfer/matrix
// ---------------------------------------------------------------------------

TEST(ApplyColorMetadataToNvenc, Av1_FullRange_Bt709) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Av1, MakeSdrColor(ColorRange::Full));
    const auto& av1 = cfg.encodeCodecConfig.av1Config;
    EXPECT_EQ(av1.colorPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(av1.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(av1.matrixCoefficients, NV_ENC_VUI_MATRIX_COEFFS_BT709);
    EXPECT_EQ(av1.colorRange, 1u) << "AV1 colorRange must be 1 (full swing) for ColorRange::Full";
}

TEST(ApplyColorMetadataToNvenc, Av1_LimitedRange_Bt709) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Av1, MakeSdrColor(ColorRange::Limited));
    const auto& av1 = cfg.encodeCodecConfig.av1Config;
    EXPECT_EQ(av1.colorPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(av1.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(av1.matrixCoefficients, NV_ENC_VUI_MATRIX_COEFFS_BT709);
    EXPECT_EQ(av1.colorRange, 0u) << "AV1 colorRange must be 0 (studio swing) for ColorRange::Limited";
}

TEST(ApplyColorMetadataToNvenc, Av1_NonDefaultColor_Bt2020Pq) {
    // Round-trips arbitrary (e.g. future HDR) ColorMetadata values too — the
    // mapping is generic, not hardcoded to BT.709.
    NV_ENC_CONFIG cfg{};
    ColorMetadata color;
    color.primaries = ColorPrimaries::Bt2020;
    color.transfer = TransferCharacteristics::SmpteSt2084;
    color.matrix = MatrixCoefficients::Bt2020Ncl;
    color.range = ColorRange::Full;
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Av1, color);
    const auto& av1 = cfg.encodeCodecConfig.av1Config;
    EXPECT_EQ(av1.colorPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT2020);
    EXPECT_EQ(av1.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084);
    EXPECT_EQ(av1.matrixCoefficients, NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL);
    EXPECT_EQ(av1.colorRange, 1u);
}

// ---------------------------------------------------------------------------
// H.264 — VUI parameters
// ---------------------------------------------------------------------------

TEST(ApplyColorMetadataToNvenc, H264_FullRange_Bt709) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::H264, MakeSdrColor(ColorRange::Full));
    const auto& vui = cfg.encodeCodecConfig.h264Config.h264VUIParameters;
    EXPECT_EQ(vui.videoSignalTypePresentFlag, 1u);
    EXPECT_EQ(vui.videoFormat, NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED);
    EXPECT_EQ(vui.videoFullRangeFlag, 1u) << "videoFullRangeFlag must be 1 (full) for ColorRange::Full";
    EXPECT_EQ(vui.colourDescriptionPresentFlag, 1u);
    EXPECT_EQ(vui.colourPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(vui.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(vui.colourMatrix, NV_ENC_VUI_MATRIX_COEFFS_BT709);
}

TEST(ApplyColorMetadataToNvenc, H264_LimitedRange_Bt709) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::H264, MakeSdrColor(ColorRange::Limited));
    const auto& vui = cfg.encodeCodecConfig.h264Config.h264VUIParameters;
    EXPECT_EQ(vui.videoSignalTypePresentFlag, 1u);
    EXPECT_EQ(vui.videoFullRangeFlag, 0u) << "videoFullRangeFlag must be 0 (limited) for ColorRange::Limited";
    EXPECT_EQ(vui.colourDescriptionPresentFlag, 1u);
    EXPECT_EQ(vui.colourPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(vui.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(vui.colourMatrix, NV_ENC_VUI_MATRIX_COEFFS_BT709);
}

// ---------------------------------------------------------------------------
// HEVC — same VUI struct layout as H.264 (NV_ENC_CONFIG_HEVC_VUI_PARAMETERS is
// a typedef of NV_ENC_CONFIG_H264_VUI_PARAMETERS), stored in hevcConfig.
// ---------------------------------------------------------------------------

TEST(ApplyColorMetadataToNvenc, Hevc_FullRange_Bt709) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Hevc, MakeSdrColor(ColorRange::Full));
    const auto& vui = cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters;
    EXPECT_EQ(vui.videoSignalTypePresentFlag, 1u);
    EXPECT_EQ(vui.videoFormat, NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED);
    EXPECT_EQ(vui.videoFullRangeFlag, 1u);
    EXPECT_EQ(vui.colourDescriptionPresentFlag, 1u);
    EXPECT_EQ(vui.colourPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(vui.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(vui.colourMatrix, NV_ENC_VUI_MATRIX_COEFFS_BT709);
}

TEST(ApplyColorMetadataToNvenc, Hevc_LimitedRange_Bt709) {
    NV_ENC_CONFIG cfg{};
    ApplyColorMetadataToNvenc(cfg, VideoCodec::Hevc, MakeSdrColor(ColorRange::Limited));
    const auto& vui = cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters;
    EXPECT_EQ(vui.videoSignalTypePresentFlag, 1u);
    EXPECT_EQ(vui.videoFullRangeFlag, 0u);
    EXPECT_EQ(vui.colourDescriptionPresentFlag, 1u);
    EXPECT_EQ(vui.colourPrimaries, NV_ENC_VUI_COLOR_PRIMARIES_BT709);
    EXPECT_EQ(vui.transferCharacteristics, NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709);
    EXPECT_EQ(vui.colourMatrix, NV_ENC_VUI_MATRIX_COEFFS_BT709);
}

} // namespace recorder_core
