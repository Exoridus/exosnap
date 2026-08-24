#pragma once

#include <cstdint>
#include <vector>

#include "color_metadata.h"

// In-band (per-stream) HDR10 static metadata payload builders.
//
// HDR10 signaling previously existed only at the container level (Matroska
// Colour / MasterMetadata, MP4 colr/mdcv boxes) plus the VUI / color_config in
// the bitstream. Some players — notably Apple's — ignore container-level HDR
// metadata and rely on in-band messages. These builders produce the byte-exact
// payloads for:
//
//   * HEVC: Mastering Display Colour Volume SEI (payload type 137, H.265 D.2.28
//     / D.3.28) and Content Light Level Info SEI (payload type 144, H.265
//     D.2.35). The returned bytes are the SEI *payload* content only — no NAL
//     header, no payloadType/payloadSize prefix, no emulation-prevention. NVENC
//     wraps them into a PREFIX_SEI NAL when handed via
//     NV_ENC_PIC_PARAMS_HEVC::seiPayloadArray with the matching payloadType.
//
//   * AV1: metadata payloads for METADATA_TYPE_HDR_MDCV (2) and
//     METADATA_TYPE_HDR_CLL (1) (AV1 spec 6.7.3 / 6.7.4). The returned bytes are
//     the metadata *payload* content only — no obu_header, no obu_size, and no
//     metadata_type leb128. NVENC builds the metadata OBU (header, leb128 size,
//     metadata_type from the entry's payloadType, and trailing byte alignment)
//     when handed via NV_ENC_PIC_PARAMS_AV1::obuPayloadArray.
//
// The two codecs deliberately differ in encoding (this is why one shared struct
// is not enough):
//   * primary order: HEVC is Green, Blue, Red (D.3.28); AV1 is Red, Green, Blue
//     (AV1 6.7.4).
//   * chromaticity units: HEVC display_primaries are in 0.00002 units
//     (value = round(coord * 50000)); AV1 primary/white chromaticity is 0.16
//     fixed-point (value = round(coord * 65536)).
//   * luminance units: HEVC max/min display luminance are u(32) in 0.0001 cd/m^2
//     units (value = round(nits * 10000)); AV1 luminance_max is 24.8 fixed-point
//     (value = round(nits * 256)) and luminance_min is 18.14 fixed-point
//     (value = round(nits * 16384)).
//   * content light level (CLL) is identical for both: two u(16) values
//     (max_content_light_level / MaxCLL, then max_pic_average / MaxFALL).
//
// All multi-byte fields are big-endian. Every function is pure and headless —
// no GPU, no NVENC session — so the exact byte output is unit-tested against
// hand-computed vectors.

namespace exosnap::engine::hdr_meta {

// HEVC SEI payload types (H.265 Annex D). Used as NV_ENC_SEI_PAYLOAD::payloadType.
inline constexpr uint32_t kHevcSeiPayloadTypeMasteringDisplay = 137;
inline constexpr uint32_t kHevcSeiPayloadTypeContentLightLevel = 144;

// AV1 metadata types (AV1 spec 6.7.1). Used as NV_ENC_AV1_OBU_PAYLOAD::payloadType
// (NVENC writes it as the metadata OBU's metadata_type leb128).
inline constexpr uint32_t kAv1MetadataTypeHdrCll = 1;
inline constexpr uint32_t kAv1MetadataTypeHdrMdcv = 2;

// True when the session encodes an HDR10-native (PQ / BT.2020) stream that
// actually carries mastering-display and/or content-light data. SDR and
// tone-map-SDR sessions (hdr == false) return false, so their bitstream stays
// byte-identical to before this feature. The caller additionally restricts
// emission to HEVC / AV1 (H.264 + HDR-native is a diagnostic blocker).
[[nodiscard]] bool ShouldEmitHdrBitstreamMetadata(const ColorMetadata& color) noexcept;

// Mastering-display data is present (its own explicit flag; a 0.0 chromaticity
// is not a meaningful "unset" sentinel — see color_metadata.h).
[[nodiscard]] bool HasMasteringDisplayData(const ColorMetadata& color) noexcept;

// Content-light data is present (MaxCLL or MaxFALL non-zero; 0 == absent).
[[nodiscard]] bool HasContentLightLevelData(const ColorMetadata& color) noexcept;

// HEVC Mastering Display Colour Volume SEI payload (24 bytes, H.265 D.2.28).
[[nodiscard]] std::vector<uint8_t> BuildHevcMasteringDisplaySeiPayload(const ColorMetadata& color);

// HEVC Content Light Level Info SEI payload (4 bytes, H.265 D.2.35).
[[nodiscard]] std::vector<uint8_t> BuildHevcContentLightLevelSeiPayload(const ColorMetadata& color);

// AV1 HDR MDCV metadata payload (24 bytes, AV1 6.7.4).
[[nodiscard]] std::vector<uint8_t> BuildAv1MasteringDisplayObuPayload(const ColorMetadata& color);

// AV1 HDR CLL metadata payload (4 bytes, AV1 6.7.3).
[[nodiscard]] std::vector<uint8_t> BuildAv1ContentLightLevelObuPayload(const ColorMetadata& color);

} // namespace exosnap::engine::hdr_meta
