#pragma once

// H.264 Annex-B bitstream utilities.
// ExtractH264SpsAndPps: scan for SPS (type 7) and PPS (type 8) NAL units.
// Used to populate MF_MT_MPEG_SEQUENCE_HEADER before starting IMFSinkWriter.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recorder_core::annexb {

// Scan an Annex-B H264 packet for the first SPS and first PPS NAL unit.
// On success, out contains: {0x00,0x00,0x00,0x01,<SPS>} + {0x00,0x00,0x00,0x01,<PPS>}.
// Returns false if either NAL is missing.
bool ExtractH264SpsAndPps(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

} // namespace recorder_core::annexb
