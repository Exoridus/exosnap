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

// ---------------------------------------------------------------------------
// HEVC SPS RBSP parser (for hvcC fixed-header fields)
// ---------------------------------------------------------------------------
//
// The hvcC fixed header must reflect the real general_profile/tier/level, the
// chroma_format_idc and the luma/chroma bit depth so a 10-bit (Main10) stream is
// tagged correctly. These come from the SPS RBSP, which is a bit-packed
// (non-byte-aligned) syntax using Exp-Golomb ue(v) codes. We strip
// emulation-prevention bytes (0x00 0x00 0x03 → 0x00 0x00) and read the prefix of
// the SPS up to bit_depth_chroma_minus8. See H.265 (ITU-T H.265) 7.3.2.2.1.

struct ParsedHevcSps {
    uint8_t profile_space = 0;          // general_profile_space (2 bits)
    uint8_t tier_flag = 0;              // general_tier_flag (1 bit)
    uint8_t profile_idc = 1;            // general_profile_idc (5 bits) — Main=1, Main10=2
    uint32_t profile_compatibility = 0; // general_profile_compatibility_flags (32 bits)
    uint8_t constraint_bytes[6] = {0};  // 6 bytes of constraint_indicator flags
    uint8_t level_idc = 0;              // general_level_idc (8 bits)
    uint32_t chroma_format_idc = 1;     // 1 = 4:2:0
    uint32_t bit_depth_luma_minus8 = 0; // 0 = 8-bit, 2 = 10-bit
    uint32_t bit_depth_chroma_minus8 = 0;
};

// Minimal MSB-first bit reader over an RBSP with emulation-prevention removal.
class RbspBitReader {
  public:
    RbspBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {
    }

    bool ReadBit(uint32_t& out) {
        if (byte_pos_ >= size_)
            return false;
        const uint8_t byte = data_[byte_pos_];
        out = (byte >> (7u - bit_pos_)) & 0x1u;
        if (++bit_pos_ == 8u) {
            bit_pos_ = 0u;
            ++byte_pos_;
            // Skip the emulation_prevention_three_byte: 0x00 0x00 0x03 → the 0x03
            // is not part of the RBSP payload. Detect when the next byte is 0x03
            // and the two bytes before it (within the NAL) were both 0x00.
            if (byte_pos_ >= 2u && byte_pos_ < size_ && data_[byte_pos_] == 0x03u && data_[byte_pos_ - 1u] == 0x00u &&
                data_[byte_pos_ - 2u] == 0x00u) {
                ++byte_pos_;
            }
        }
        return true;
    }

    bool ReadBits(uint32_t count, uint32_t& out) {
        out = 0u;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t bit = 0;
            if (!ReadBit(bit))
                return false;
            out = (out << 1u) | bit;
        }
        return true;
    }

    // Exp-Golomb unsigned: count leading zero bits n, then read n more bits.
    bool ReadUe(uint32_t& out) {
        uint32_t zeros = 0;
        uint32_t bit = 0;
        while (true) {
            if (!ReadBit(bit))
                return false;
            if (bit == 1u)
                break;
            if (++zeros > 31u)
                return false; // malformed / out of range
        }
        uint32_t suffix = 0;
        if (zeros > 0u && !ReadBits(zeros, suffix))
            return false;
        out = (1u << zeros) - 1u + suffix;
        return true;
    }

  private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t byte_pos_ = 0;
    uint32_t bit_pos_ = 0;
};

// Parse profile_tier_level() for the general layer plus, when present, skip the
// sub-layer PTL. Returns false on truncation. profilePresentFlag is assumed 1
// (always the case for the SPS top-level PTL).
static bool ParseProfileTierLevel(RbspBitReader& r, uint32_t max_sub_layers_minus1, ParsedHevcSps& out) {
    uint32_t v = 0;
    if (!r.ReadBits(2u, v))
        return false;
    out.profile_space = static_cast<uint8_t>(v);
    if (!r.ReadBit(v))
        return false;
    out.tier_flag = static_cast<uint8_t>(v);
    if (!r.ReadBits(5u, v))
        return false;
    out.profile_idc = static_cast<uint8_t>(v);
    if (!r.ReadBits(32u, out.profile_compatibility))
        return false;
    // 48 bits of constraint flags / reserved = 6 bytes.
    for (int i = 0; i < 6; ++i) {
        if (!r.ReadBits(8u, v))
            return false;
        out.constraint_bytes[i] = static_cast<uint8_t>(v);
    }
    if (!r.ReadBits(8u, v))
        return false;
    out.level_idc = static_cast<uint8_t>(v);

    // Sub-layer PTL. For NVENC HEVC output max_sub_layers_minus1 is 0, so this is
    // usually a no-op, but parse it correctly for robustness.
    if (max_sub_layers_minus1 > 0u) {
        uint8_t sub_profile_present[8] = {0};
        uint8_t sub_level_present[8] = {0};
        for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
            uint32_t pp = 0, lp = 0;
            if (!r.ReadBit(pp) || !r.ReadBit(lp))
                return false;
            sub_profile_present[i] = static_cast<uint8_t>(pp);
            sub_level_present[i] = static_cast<uint8_t>(lp);
        }
        if (max_sub_layers_minus1 > 0u && max_sub_layers_minus1 < 8u) {
            // reserved_zero_2bits for i in [max_sub_layers_minus1, 8)
            for (uint32_t i = max_sub_layers_minus1; i < 8u; ++i) {
                if (!r.ReadBits(2u, v))
                    return false;
            }
        }
        for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
            if (sub_profile_present[i]) {
                // 2+1+5 + 32 + 48 = 88 bits of sub-layer profile fields
                if (!r.ReadBits(8u, v) || !r.ReadBits(32u, v))
                    return false;
                for (int k = 0; k < 6; ++k) {
                    if (!r.ReadBits(8u, v))
                        return false;
                }
            }
            if (sub_level_present[i]) {
                if (!r.ReadBits(8u, v)) // sub_layer_level_idc
                    return false;
            }
        }
    }
    return true;
}

// Parse the prefix of an HEVC SPS NAL (including the 2-byte NAL header) up to and
// including bit_depth_chroma_minus8. Returns false on truncation/malformed input;
// the caller then falls back to conservative defaults (Main 8-bit 4:2:0).
static bool ParseHevcSps(const uint8_t* sps_nal, size_t sps_len, ParsedHevcSps& out) {
    if (sps_nal == nullptr || sps_len < 3u)
        return false;
    // Skip the 2-byte NAL header; the RBSP starts at sps_nal[2].
    RbspBitReader r(sps_nal + 2u, sps_len - 2u);

    uint32_t v = 0;
    if (!r.ReadBits(4u, v)) // sps_video_parameter_set_id
        return false;
    uint32_t max_sub_layers_minus1 = 0;
    if (!r.ReadBits(3u, max_sub_layers_minus1)) // sps_max_sub_layers_minus1
        return false;
    if (!r.ReadBit(v)) // sps_temporal_id_nesting_flag
        return false;

    if (!ParseProfileTierLevel(r, max_sub_layers_minus1, out))
        return false;

    uint32_t sps_seq_parameter_set_id = 0;
    if (!r.ReadUe(sps_seq_parameter_set_id))
        return false;
    if (!r.ReadUe(out.chroma_format_idc))
        return false;
    if (out.chroma_format_idc == 3u) {
        if (!r.ReadBit(v)) // separate_colour_plane_flag
            return false;
    }
    uint32_t pic_w = 0, pic_h = 0;
    if (!r.ReadUe(pic_w) || !r.ReadUe(pic_h))
        return false;
    uint32_t conformance_window_flag = 0;
    if (!r.ReadBit(conformance_window_flag))
        return false;
    if (conformance_window_flag) {
        uint32_t dummy = 0;
        if (!r.ReadUe(dummy) || !r.ReadUe(dummy) || !r.ReadUe(dummy) || !r.ReadUe(dummy))
            return false;
    }
    if (!r.ReadUe(out.bit_depth_luma_minus8))
        return false;
    if (!r.ReadUe(out.bit_depth_chroma_minus8))
        return false;
    return true;
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

    // Parse the SPS RBSP for the fixed-header fields that vary by profile/bit depth:
    // general_profile/tier/level, chroma_format_idc, and the luma/chroma bit depths.
    // These must reflect the real stream so 10-bit (Main10) is tagged correctly; a
    // hardcoded 8-bit Main hvcC on a Main10 stream is invalid metadata. On any parse
    // failure we fall back to the historic conservative Main-8-bit-4:2:0 constants so
    // a malformed or unexpected SPS still yields a usable 8-bit hvcC.
    ParsedHevcSps sps{};
    const bool sps_parsed = ParseHevcSps(sps_data, sps_len, sps);

    // Fallbacks mirror the previous hardcoded values (Main, level from byte 14 if present).
    uint8_t profile_space = 0u;
    uint8_t tier_flag = 0u;
    uint8_t profile_idc = 1u; // Main
    uint32_t profile_compatibility = 0x60000000u;
    uint8_t constraint_bytes[6] = {0x90u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    uint8_t general_level_idc = (sps_len >= 15u) ? sps_data[14] : 0u;
    uint32_t chroma_format_idc = 1u;       // 4:2:0
    uint32_t bit_depth_luma_minus8 = 0u;   // 8-bit
    uint32_t bit_depth_chroma_minus8 = 0u; // 8-bit

    if (sps_parsed) {
        profile_space = sps.profile_space;
        tier_flag = sps.tier_flag;
        profile_idc = sps.profile_idc;
        profile_compatibility = sps.profile_compatibility;
        for (int i = 0; i < 6; ++i)
            constraint_bytes[i] = sps.constraint_bytes[i];
        general_level_idc = sps.level_idc;
        chroma_format_idc = sps.chroma_format_idc;
        bit_depth_luma_minus8 = sps.bit_depth_luma_minus8;
        bit_depth_chroma_minus8 = sps.bit_depth_chroma_minus8;
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

    // general_profile_space(2b) + general_tier_flag(1b) + general_profile_idc(5b)
    // From SPS: Main=1 (→ 0x01), Main10=2 (→ 0x02).
    out_hvcc.push_back(
        static_cast<uint8_t>(((profile_space & 0x3u) << 6u) | ((tier_flag & 0x1u) << 5u) | (profile_idc & 0x1Fu)));

    // general_profile_compatibility_flags (4 bytes, big-endian) — from SPS.
    out_hvcc.push_back(static_cast<uint8_t>((profile_compatibility >> 24u) & 0xFFu));
    out_hvcc.push_back(static_cast<uint8_t>((profile_compatibility >> 16u) & 0xFFu));
    out_hvcc.push_back(static_cast<uint8_t>((profile_compatibility >> 8u) & 0xFFu));
    out_hvcc.push_back(static_cast<uint8_t>(profile_compatibility & 0xFFu));

    // general_constraint_indicator_flags (6 bytes) — from SPS PTL.
    for (int i = 0; i < 6; ++i)
        out_hvcc.push_back(constraint_bytes[i]);

    // general_level_idc
    out_hvcc.push_back(general_level_idc);

    // min_spatial_segmentation_idc: reserved(4b=0xF) + value(12b=0) = 0xF000
    out_hvcc.push_back(0xF0u);
    out_hvcc.push_back(0x00u);

    // parallelismType: reserved(6b=0x3F) + type(2b=0) = 0xFC
    out_hvcc.push_back(0xFCu);

    // chroma_format_idc: reserved(6b=0x3F) + chromaFormat(2b) — from SPS (1 = 4:2:0 → 0xFD)
    out_hvcc.push_back(static_cast<uint8_t>(0xFCu | (chroma_format_idc & 0x3u)));

    // bit_depth_luma_minus8: reserved(5b=0x1F) + value(3b) — from SPS (0 = 8-bit → 0xF8, 2 = 10-bit → 0xFA)
    out_hvcc.push_back(static_cast<uint8_t>(0xF8u | (bit_depth_luma_minus8 & 0x7u)));

    // bit_depth_chroma_minus8: reserved(5b=0x1F) + value(3b) — from SPS (0 = 8-bit → 0xF8, 2 = 10-bit → 0xFA)
    out_hvcc.push_back(static_cast<uint8_t>(0xF8u | (bit_depth_chroma_minus8 & 0x7u)));

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
