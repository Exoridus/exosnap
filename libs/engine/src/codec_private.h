#pragma once

// Internal helpers for codec-private data derivation.
// Lifted from M2.8 probe (probe_wgc_nvenc_aac_mkv).

#include <cstddef>
#include <cstdint>
#include <vector>

#include <mfidl.h>
#include <windows.h>

namespace exosnap::engine::codec_private {

// Parse an AV1 bitstream packet and derive the 4-byte
// AV1CodecConfigurationRecord for Matroska CodecPrivate.
// Returns true on success and writes 4 bytes to out_av1_codec_private.
// On failure, writes a short diagnostic to reason_buf.
bool DeriveAv1CodecPrivate(const uint8_t* bitstream_data, size_t bitstream_size,
                           std::vector<uint8_t>& out_av1_codec_private, char* reason_buf, size_t reason_buf_size);

// Builds the 19-byte OpusHead (Matroska A_OPUS CodecPrivate).
// pre_skip: encoder lookahead from OPUS_GET_LOOKAHEAD.
// out_19 must point to at least 19 bytes.
void BuildOpusCodecPrivate(uint32_t sample_rate, uint32_t channels, uint16_t pre_skip, uint8_t* out_19);

} // namespace exosnap::engine::codec_private
