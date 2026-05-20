#pragma once

// Internal helpers for codec-private data derivation.
// Lifted from M2.8 probe (probe_wgc_nvenc_aac_mkv).

#include <cstddef>
#include <cstdint>

#include <mfidl.h>
#include <windows.h>

namespace recorder_core::codec_private {

// Parse an AV1 bitstream packet and derive the 4-byte
// AV1CodecConfigurationRecord for Matroska CodecPrivate.
// Returns true on success and writes 4 bytes to out_av1_codec_private.
// On failure, writes a short diagnostic to reason_buf.
bool DeriveAv1CodecPrivate(const uint8_t* bitstream_data, size_t bitstream_size, uint8_t out_av1_codec_private[4],
                           char* reason_buf, size_t reason_buf_size);

// Extract the 2-byte AudioSpecificConfig from the MF_MT_USER_DATA blob
// on the AAC encoder output media type.
// Returns true on success and writes 2 bytes to out_aac_codec_private.
// On failure, writes a short diagnostic to reason_buf.
bool DeriveAacCodecPrivate(IMFMediaType* output_media_type, uint8_t out_aac_codec_private[2], char* reason_buf,
                           size_t reason_buf_size);

// Builds the 19-byte OpusHead (Matroska A_OPUS CodecPrivate).
// pre_skip: encoder lookahead from OPUS_GET_LOOKAHEAD.
// out_19 must point to at least 19 bytes.
void BuildOpusCodecPrivate(uint32_t sample_rate, uint32_t channels, uint16_t pre_skip, uint8_t* out_19);

} // namespace recorder_core::codec_private
