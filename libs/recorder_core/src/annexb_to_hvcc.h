#pragma once

// HEVC Annex-B bitstream utilities.
// ExtractHevcVpsSpsPps: scan for VPS (type 32), SPS (type 33), and PPS (type 34) NAL units.
// BuildHvccFromAnnexBVpsSpsPps: build HEVCDecoderConfigurationRecord for Matroska V_MPEGH/ISO/HEVC.
// ConvertAnnexBToHvcC_Sample: convert a complete Annex-B HEVC frame to length-prefixed sample payload.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recorder_core::annexb {

// HEVC NAL unit header is 2 bytes.
// NAL type = (header_byte0 >> 1) & 0x3F
// VPS = 32, SPS = 33, PPS = 34, AUD = 35
constexpr uint8_t kHevcNalVps = 32u;
constexpr uint8_t kHevcNalSps = 33u;
constexpr uint8_t kHevcNalPps = 34u;
constexpr uint8_t kHevcNalAud = 35u;

// Scan an Annex-B HEVC packet for the first VPS, SPS, and PPS NAL units.
// On success, out contains:
//   {0x00,0x00,0x00,0x01,<VPS NAL>}
//   {0x00,0x00,0x00,0x01,<SPS NAL>}
//   {0x00,0x00,0x00,0x01,<PPS NAL>}
// Returns false if any of VPS, SPS, or PPS is missing.
bool ExtractHevcVpsSpsPps(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

// Build an HEVCDecoderConfigurationRecord (hvcC) from Annex-B VPS+SPS+PPS data.
// Input: vps_sps_pps_annexb must be the output of ExtractHevcVpsSpsPps
//        (three NAL units, each preceded by a 4-byte start code 00 00 00 01).
// Output: out_hvcc receives the complete HEVCDecoderConfigurationRecord.
// Returns false if input is too short or malformed.
//
// hvcC binary layout (ISO 14496-15 section 8.3.3.1.2):
//   configurationVersion       (1 byte)  = 0x01
//   general_profile_space etc  (1 byte)  = 0x01 (profile_space=0, tier=0, profile_idc=1 Main)
//   general_profile_compat     (4 bytes) = 0x60000000 (Main / Main Still)
//   general_constraint_flags   (6 bytes) = 0x90000000000000 (progressive, interlaced, one_picture_only off)
//     Note: we write 6 bytes of constraint_indicator flags + level byte
//   general_level_idc          (1 byte)  from SPS (byte at offset 13 in RBSP, conservative fallback 0)
//   min_spatial_segmentation   (2 bytes) = 0xF000 (reserved=0xF, spatial=0)
//   parallelism                (1 byte)  = 0xFC (reserved=0xFC, parallelismType=0)
//   chromaFormat               (1 byte)  = 0xFD (reserved=0xFC, chromaFormat=1 = 4:2:0)
//   bitDepthLumaMinus8         (1 byte)  = 0xF8 (reserved=0xF8, bitDepth=0 = 8-bit)
//   bitDepthChromaMinus8       (1 byte)  = 0xF8
//   avgFrameRate               (2 bytes) = 0x0000
//   misc                       (1 byte)  = 0x0F
//   (constantFrameRate=0,numTemporalLayers=1,temporalIdNested=1,lengthSizeMinusOne=3) numOfArrays                (1
//   byte)  = 3 (VPS, SPS, PPS) For each NAL array:
//     array_completeness+NAL_unit_type (1 byte)
//     numNalus                          (2 bytes)
//     naluLength                        (2 bytes)
//     naluData                          (naluLength bytes)
bool BuildHvccFromAnnexBVpsSpsPps(const std::vector<uint8_t>& vps_sps_pps_annexb, std::vector<uint8_t>& out_hvcc);

// Convert a complete Annex-B HEVC bitstream packet to a length-prefixed sample payload.
// Each NAL unit is written as a 4-byte big-endian length prefix followed by the
// NAL payload (start codes removed). AUD NALs (type 35) are skipped.
// Required for Matroska V_MPEGH/ISO/HEVC sample data.
// Returns false if data is null/empty or contains no convertible NAL units.
bool ConvertAnnexBToHevcSample(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

} // namespace recorder_core::annexb
