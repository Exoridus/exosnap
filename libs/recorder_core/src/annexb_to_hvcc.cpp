#include "annexb_to_hvcc.h"

#include <cstdio>

namespace recorder_core::annexb {

namespace {

// Returns the length of the start code at position i (3 or 4), or 0 if none.
static int HevcStartCodeLen(const uint8_t* data, size_t size, size_t i) noexcept {
    if (i + 3 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        return 4;
    if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        return 3;
    return 0;
}

struct HevcNalSpan {
    size_t payload_offset = 0; // offset of first byte after start code (first NAL header byte)
    size_t payload_size = 0;
    uint8_t nal_type = 0; // (header_byte0 >> 1) & 0x3F
};

// Enumerate all NAL units in an Annex-B HEVC bitstream.
// HEVC NAL unit header: 2 bytes.
//   Byte 0: forbidden_zero_bit(1) + nal_unit_type(6) + nuh_layer_id_msb(1)
//   Byte 1: nuh_layer_id_lsb(5) + nuh_temporal_id_plus1(3)
// nal_type = (byte0 >> 1) & 0x3F
static void EnumerateHevcNals(const uint8_t* data, size_t size, std::vector<HevcNalSpan>& out) {
    size_t i = 0;
    while (i < size) {
        int scLen = HevcStartCodeLen(data, size, i);
        if (scLen == 0) {
            ++i;
            continue;
        }
        size_t payloadStart = i + static_cast<size_t>(scLen);
        if (payloadStart >= size)
            break;

        // Need at least 2 bytes for HEVC NAL header
        if (payloadStart + 1 >= size) {
            break;
        }

        // Find next start code to determine end of this NAL
        size_t payloadEnd = size;
        for (size_t j = payloadStart + 2; j < size; ++j) {
            if (HevcStartCodeLen(data, size, j) > 0) {
                payloadEnd = j;
                break;
            }
        }

        HevcNalSpan span;
        span.payload_offset = payloadStart;
        span.payload_size = payloadEnd - payloadStart;
        span.nal_type = static_cast<uint8_t>((data[payloadStart] >> 1u) & 0x3Fu);
        out.push_back(span);

        i = payloadEnd;
    }
}

} // namespace

bool ExtractHevcVpsSpsPps(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (data == nullptr && size > 0)
        return false;

    std::vector<HevcNalSpan> nals;
    EnumerateHevcNals(data, size, nals);

    const HevcNalSpan* vps = nullptr;
    const HevcNalSpan* sps = nullptr;
    const HevcNalSpan* pps = nullptr;

    for (const auto& nal : nals) {
        if (nal.nal_type == kHevcNalVps && vps == nullptr)
            vps = &nal;
        if (nal.nal_type == kHevcNalSps && sps == nullptr)
            sps = &nal;
        if (nal.nal_type == kHevcNalPps && pps == nullptr)
            pps = &nal;
        if (vps && sps && pps)
            break;
    }

    if (!vps || !sps || !pps)
        return false;

    out.clear();
    out.reserve(4 + vps->payload_size + 4 + sps->payload_size + 4 + pps->payload_size);
    out.insert(out.end(), {0x00u, 0x00u, 0x00u, 0x01u});
    out.insert(out.end(), data + vps->payload_offset, data + vps->payload_offset + vps->payload_size);
    out.insert(out.end(), {0x00u, 0x00u, 0x00u, 0x01u});
    out.insert(out.end(), data + sps->payload_offset, data + sps->payload_offset + sps->payload_size);
    out.insert(out.end(), {0x00u, 0x00u, 0x00u, 0x01u});
    out.insert(out.end(), data + pps->payload_offset, data + pps->payload_offset + pps->payload_size);
    return true;
}

bool BuildHvccFromAnnexBVpsSpsPps(const std::vector<uint8_t>& vps_sps_pps_annexb, std::vector<uint8_t>& out_hvcc) {
    // Layout (from ExtractHevcVpsSpsPps):
    //   [00 00 00 01] [VPS NAL bytes...] [00 00 00 01] [SPS NAL bytes...] [00 00 00 01] [PPS NAL bytes...]
    // HEVC NAL header is 2 bytes; minimum NAL payload is 2 bytes.
    // Minimum: SC(4)+VPS(>=2) + SC(4)+SPS(>=2) + SC(4)+PPS(>=2) = 18 bytes
    constexpr size_t kMinimumSize = 18u;
    if (vps_sps_pps_annexb.size() < kMinimumSize) {
        return false;
    }

    // Verify first start code
    if (vps_sps_pps_annexb[0] != 0x00 || vps_sps_pps_annexb[1] != 0x00 || vps_sps_pps_annexb[2] != 0x00 ||
        vps_sps_pps_annexb[3] != 0x01) {
        return false;
    }

    // Parse all three NALs by re-enumerating from the vector
    std::vector<HevcNalSpan> nals;
    EnumerateHevcNals(vps_sps_pps_annexb.data(), vps_sps_pps_annexb.size(), nals);

    const HevcNalSpan* vps_span = nullptr;
    const HevcNalSpan* sps_span = nullptr;
    const HevcNalSpan* pps_span = nullptr;

    for (const auto& nal : nals) {
        if (nal.nal_type == kHevcNalVps && vps_span == nullptr)
            vps_span = &nal;
        if (nal.nal_type == kHevcNalSps && sps_span == nullptr)
            sps_span = &nal;
        if (nal.nal_type == kHevcNalPps && pps_span == nullptr)
            pps_span = &nal;
        if (vps_span && sps_span && pps_span)
            break;
    }

    if (!vps_span || !sps_span || !pps_span) {
        return false;
    }

    const size_t vps_len = vps_span->payload_size;
    const size_t sps_len = sps_span->payload_size;
    const size_t pps_len = pps_span->payload_size;

    // Each NAL must have at least 2 bytes (HEVC NAL header) and fit in uint16
    if (vps_len < 2 || sps_len < 2 || pps_len < 2) {
        return false;
    }
    if (vps_len > 0xFFFF || sps_len > 0xFFFF || pps_len > 0xFFFF) {
        return false;
    }

    const uint8_t* vps_data = vps_sps_pps_annexb.data() + vps_span->payload_offset;
    const uint8_t* sps_data = vps_sps_pps_annexb.data() + sps_span->payload_offset;
    const uint8_t* pps_data = vps_sps_pps_annexb.data() + pps_span->payload_offset;

    // Try to extract general_level_idc from SPS RBSP.
    // HEVC SPS RBSP starts at offset 2 (after 2-byte NAL header):
    //   sps_video_parameter_set_id (4 bits)
    //   sps_max_sub_layers_minus1  (3 bits)
    //   sps_temporal_id_nesting_flag (1 bit)
    //   profile_tier_level() — the general_level_idc is at byte offset 13 from the RBSP start
    //     (after general_profile_space(2) + general_tier_flag(1) + general_profile_idc(5) +
    //      general_profile_compatibility_flags(32) + general_progressive_source_flag(1) +
    //      general_interlaced_source_flag(1) + general_non_packed_constraint_flag(1) +
    //      general_frame_only_constraint_flag(1) + general_reserved_zero_44bits(44) =
    //      2+1+5+32+1+1+1+1+44 = 88 bits = 11 bytes; then general_level_idc at byte 11 from start of PTL)
    //   PTL starts at RBSP byte 2 (after NAL header + sps_video_parameter_set_id etc. byte)
    //   Conservative: we write level_idc=0 unless we can read it cleanly.
    uint8_t general_level_idc = 0u;
    // RBSP starts at sps_data[2] (sps_data[0..1] = 2-byte HEVC NAL header).
    // Byte layout in profile_tier_level (byte-aligned fields for the general_ part):
    //   PTL offset 0: general_profile_space(2b) | general_tier_flag(1b) | general_profile_idc(5b)
    //   PTL offset 1-4: general_profile_compatibility_flags (32 bits)
    //   PTL offset 5: general_progressive(1b)|interlaced(1b)|non_packed(1b)|frame_only(1b)|44 reserved bits
    //   PTL offset 5-10: 44 reserved bits span bytes 5-10 (6 bytes)
    //   PTL offset 11: general_level_idc (8 bits)
    // SPS RBSP: [sps_video_parameter_set_id(4b)|sps_max_sub_layers_minus1(3b)|temporal(1b)] [PTL starts byte 1]
    // So PTL starts at sps_data[2] byte index 1 → sps_data[3].
    // general_level_idc = sps_data[3 + 11] = sps_data[14]
    if (sps_len >= 15u) {
        general_level_idc = sps_data[14];
    }

    // ---------------------------------------------------------------------------
    // HEVCDecoderConfigurationRecord (ISO 14496-15:2019 section 8.3.3.1)
    // ---------------------------------------------------------------------------
    //
    // Field                             Size    Value
    // configurationVersion              1       0x01
    // general_profile_space(2)          }
    // general_tier_flag(1)              } 1 byte = 0x01 (space=0, tier=main, profile_idc=1)
    // general_profile_idc(5)            }
    // general_profile_compatibility     4       0x60000000 (Main+MainStillPicture)
    // general_progressive_source_flag   }
    // general_interlaced_source_flag    } 6 bytes constraint_indicator (6 bytes)
    // general_non_packed_constraint     }
    // general_frame_only_constraint     }
    // general_reserved_zero_44bits      }
    // general_level_idc                 1       from SPS
    // min_spatial_segmentation_idc      2       0xF000 (reserved=0xF, value=0)
    // parallelismType                   1       0xFC   (reserved=0xFC, type=0 mixed)
    // chroma_format_idc                 1       0xFD   (reserved=0xFC, value=1 = 4:2:0)
    // bit_depth_luma_minus8             1       0xF8   (reserved=0xF8, value=0 = 8-bit)
    // bit_depth_chroma_minus8           1       0xF8
    // avgFrameRate                      2       0x0000
    // constantFrameRate(2)              }
    // numTemporalLayers(3)              } 1 byte = 0x0F (cfr=0, layers=1, nested=1, lenSizeMinus1=3)
    // temporalIdNested(1)               }
    // lengthSizeMinusOne(2)             }
    // numOfArrays                       1       3
    //
    // Each NAL array:
    //   array_completeness(1)|reserved(1)|nal_unit_type(6)   1 byte
    //   numNalus                                              2 bytes (big-endian)
    //   naluLength                                            2 bytes (big-endian)
    //   naluData                                              naluLength bytes

    out_hvcc.clear();

    // Fixed header: 23 bytes before NAL arrays
    out_hvcc.reserve(23u + (1u + 2u + 2u + vps_len) + (1u + 2u + 2u + sps_len) + (1u + 2u + 2u + pps_len));

    // configurationVersion = 1
    out_hvcc.push_back(0x01u);

    // general_profile_space(2b=0) + general_tier_flag(1b=0, Main) + general_profile_idc(5b=1, Main)
    out_hvcc.push_back(0x01u);

    // general_profile_compatibility_flags (4 bytes) — Main: bit 1 set, MainStillPicture: bit 3 set
    // = 0x60 00 00 00
    out_hvcc.push_back(0x60u);
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);

    // general_constraint_indicator_flags (6 bytes)
    // progressive=1, interlaced=0, non_packed=0, frame_only=1, 44 reserved bits = 0
    // Byte 0: 1001 0000 = 0x90
    out_hvcc.push_back(0x90u);
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);

    // general_level_idc
    out_hvcc.push_back(general_level_idc);

    // min_spatial_segmentation_idc: reserved(4b=0xF) + value(12b=0) = 0xF000
    out_hvcc.push_back(0xF0u);
    out_hvcc.push_back(0x00u);

    // parallelismType: reserved(6b=0x3F) + type(2b=0) = 0xFC
    out_hvcc.push_back(0xFCu);

    // chroma_format_idc: reserved(6b=0x3F) + chromaFormat(2b=1 for 4:2:0) = 0xFD
    out_hvcc.push_back(0xFDu);

    // bit_depth_luma_minus8: reserved(5b=0x1F) + value(3b=0) = 0xF8
    out_hvcc.push_back(0xF8u);

    // bit_depth_chroma_minus8: reserved(5b=0x1F) + value(3b=0) = 0xF8
    out_hvcc.push_back(0xF8u);

    // avgFrameRate (2 bytes): 0 = unspecified
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x00u);

    // constantFrameRate(2b=0) + numTemporalLayers(3b=1) + temporalIdNested(1b=1) + lengthSizeMinusOne(2b=3)
    // = 0b00 001 1 11 = 0x0F
    out_hvcc.push_back(0x0Fu);

    // numOfArrays = 3 (VPS, SPS, PPS)
    out_hvcc.push_back(0x03u);

    // --- VPS array ---
    // array_completeness(1b=1) + reserved(1b=0) + nal_unit_type(6b=32=0x20)
    // = 0b1_0_100000 = 0xA0
    out_hvcc.push_back(0xA0u);
    // numNalus = 1
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x01u);
    // naluLength (big-endian 2 bytes)
    out_hvcc.push_back(static_cast<uint8_t>((vps_len >> 8u) & 0xFFu));
    out_hvcc.push_back(static_cast<uint8_t>(vps_len & 0xFFu));
    // NAL data
    out_hvcc.insert(out_hvcc.end(), vps_data, vps_data + vps_len);

    // --- SPS array ---
    // array_completeness(1b=1) + reserved(1b=0) + nal_unit_type(6b=33=0x21)
    // = 0b1_0_100001 = 0xA1
    out_hvcc.push_back(0xA1u);
    // numNalus = 1
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x01u);
    // naluLength
    out_hvcc.push_back(static_cast<uint8_t>((sps_len >> 8u) & 0xFFu));
    out_hvcc.push_back(static_cast<uint8_t>(sps_len & 0xFFu));
    // NAL data
    out_hvcc.insert(out_hvcc.end(), sps_data, sps_data + sps_len);

    // --- PPS array ---
    // array_completeness(1b=1) + reserved(1b=0) + nal_unit_type(6b=34=0x22)
    // = 0b1_0_100010 = 0xA2
    out_hvcc.push_back(0xA2u);
    // numNalus = 1
    out_hvcc.push_back(0x00u);
    out_hvcc.push_back(0x01u);
    // naluLength
    out_hvcc.push_back(static_cast<uint8_t>((pps_len >> 8u) & 0xFFu));
    out_hvcc.push_back(static_cast<uint8_t>(pps_len & 0xFFu));
    // NAL data
    out_hvcc.insert(out_hvcc.end(), pps_data, pps_data + pps_len);

    return true;
}

bool ConvertAnnexBToHevcSample(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (data == nullptr || size == 0)
        return false;

    std::vector<HevcNalSpan> nals;
    EnumerateHevcNals(data, size, nals);

    out.clear();
    for (const auto& nal : nals) {
        if (nal.nal_type == kHevcNalAud) // AUD — excluded from sample payload
            continue;
        if (nal.payload_size == 0)
            continue;
        const uint32_t len = static_cast<uint32_t>(nal.payload_size);
        out.push_back(static_cast<uint8_t>((len >> 24u) & 0xFFu));
        out.push_back(static_cast<uint8_t>((len >> 16u) & 0xFFu));
        out.push_back(static_cast<uint8_t>((len >> 8u) & 0xFFu));
        out.push_back(static_cast<uint8_t>(len & 0xFFu));
        out.insert(out.end(), data + nal.payload_offset, data + nal.payload_offset + nal.payload_size);
    }

    return !out.empty();
}

} // namespace recorder_core::annexb
