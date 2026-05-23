#pragma once

// H.264 Annex-B bitstream utilities.
// ExtractH264SpsAndPps: scan for SPS (type 7) and PPS (type 8) NAL units.
// BuildAvccFromAnnexBSpsAndPps: build AVCDecoderConfigurationRecord for Matroska V_MPEG4/ISO/AVC.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recorder_core::annexb {

// Scan an Annex-B H264 packet for the first SPS and first PPS NAL unit.
// On success, out contains: {0x00,0x00,0x00,0x01,<SPS>} + {0x00,0x00,0x00,0x01,<PPS>}.
// Returns false if either NAL is missing.
bool ExtractH264SpsAndPps(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

// Build an AVCDecoderConfigurationRecord from Annex-B SPS+PPS data.
// Input: sps_pps_annexb must be the output of ExtractH264SpsAndPps
//        (two NAL units, each preceded by a 4-byte start code 00 00 00 01).
// Output: out_avcc receives the complete AVCDecoderConfigurationRecord.
// Returns false if input is too short or malformed.
bool BuildAvccFromAnnexBSpsAndPps(const std::vector<uint8_t>& sps_pps_annexb, std::vector<uint8_t>& out_avcc);

} // namespace recorder_core::annexb
