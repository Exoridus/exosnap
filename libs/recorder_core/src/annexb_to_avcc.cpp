#include "annexb_to_avcc.h"

#include <cstdio>

namespace recorder_core::annexb {

namespace {

// Returns the length of the start code at position i (3 or 4), or 0 if none.
static int StartCodeLen(const uint8_t* data, size_t size, size_t i) noexcept {
    if (i + 3 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        return 4;
    if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        return 3;
    return 0;
}

struct NalSpan {
    size_t payload_offset = 0; // offset of first byte after start code
    size_t payload_size = 0;
    uint8_t nal_type = 0; // low 5 bits of first NAL byte
};

// Enumerate all NAL units in an Annex-B bitstream.
static void EnumerateNals(const uint8_t* data, size_t size, std::vector<NalSpan>& out) {
    size_t i = 0;
    while (i < size) {
        int scLen = StartCodeLen(data, size, i);
        if (scLen == 0) {
            ++i;
            continue;
        }
        size_t payloadStart = i + static_cast<size_t>(scLen);
        if (payloadStart >= size)
            break;

        // Find next start code to determine end of this NAL
        size_t payloadEnd = size;
        for (size_t j = payloadStart + 1; j < size; ++j) {
            if (StartCodeLen(data, size, j) > 0) {
                payloadEnd = j;
                break;
            }
        }

        NalSpan span;
        span.payload_offset = payloadStart;
        span.payload_size = payloadEnd - payloadStart;
        span.nal_type = data[payloadStart] & 0x1Fu;
        out.push_back(span);

        i = payloadEnd;
    }
}

} // namespace

bool ExtractH264SpsAndPps(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (data == nullptr && size > 0)
        return false;

    std::vector<NalSpan> nals;
    EnumerateNals(data, size, nals);

    const NalSpan* sps = nullptr;
    const NalSpan* pps = nullptr;

    for (const auto& nal : nals) {
        if (nal.nal_type == 7u && sps == nullptr)
            sps = &nal;
        if (nal.nal_type == 8u && pps == nullptr)
            pps = &nal;
        if (sps && pps)
            break;
    }

    if (!sps || !pps)
        return false;

    out.clear();
    out.reserve(4 + sps->payload_size + 4 + pps->payload_size);
    out.insert(out.end(), {0x00u, 0x00u, 0x00u, 0x01u});
    out.insert(out.end(), data + sps->payload_offset, data + sps->payload_offset + sps->payload_size);
    out.insert(out.end(), {0x00u, 0x00u, 0x00u, 0x01u});
    out.insert(out.end(), data + pps->payload_offset, data + pps->payload_offset + pps->payload_size);
    return true;
}

bool BuildAvccFromAnnexBSpsAndPps(const std::vector<uint8_t>& sps_pps_annexb, std::vector<uint8_t>& out_avcc) {
    // sps_pps_annexb layout (from ExtractH264SpsAndPps):
    //   [00 00 00 01] [SPS NAL bytes...] [00 00 00 01] [PPS NAL bytes...]
    //   SPS NAL first byte = 0x67 (nal_unit_type=7), followed by profile_idc, profile_compat, level_idc, ...
    //   PPS NAL first byte = 0x68 (nal_unit_type=8), followed by PPS payload

    // Minimum: SC(4) + SPS(≥4: type+profile+compat+level) + SC(4) + PPS(≥2: type+1) = 14
    constexpr size_t kMinimumSize = 14;
    if (sps_pps_annexb.size() < kMinimumSize) {
        return false;
    }

    // Skip the 4-byte start code for the SPS NAL.
    if (sps_pps_annexb[0] != 0x00 || sps_pps_annexb[1] != 0x00 || sps_pps_annexb[2] != 0x00 ||
        sps_pps_annexb[3] != 0x01) {
        return false;
    }
    const size_t sps_start = 4; // first byte of SPS NAL (0x67)

    // Find the second start code (PPS).
    constexpr size_t kNotFound = static_cast<size_t>(-1);
    size_t pps_sc_pos = kNotFound;
    for (size_t i = sps_start + 1; i + 3 < sps_pps_annexb.size(); ++i) {
        if (sps_pps_annexb[i] == 0x00 && sps_pps_annexb[i + 1] == 0x00 && sps_pps_annexb[i + 2] == 0x00 &&
            sps_pps_annexb[i + 3] == 0x01) {
            pps_sc_pos = i;
            break;
        }
    }

    if (pps_sc_pos == kNotFound) {
        return false;
    }

    const size_t sps_len = pps_sc_pos - sps_start;
    const size_t pps_start = pps_sc_pos + 4; // first byte of PPS NAL (0x68)
    const size_t pps_len = sps_pps_annexb.size() - pps_start;

    // SPS must contain at least: type(1) + profile_idc(1) + profile_compat(1) + level_idc(1)
    if (sps_len < 4 || pps_len < 1) {
        return false;
    }
    if (sps_len > 0xFFFF || pps_len > 0xFFFF) {
        return false;
    }

    const uint8_t* sps = sps_pps_annexb.data() + sps_start;
    const uint8_t* pps = sps_pps_annexb.data() + pps_start;

    // sps[0] = NAL type byte (0x67), sps[1]=profile_idc, sps[2]=profile_compat, sps[3]=level_idc
    const uint8_t profile_idc = sps[1];
    const uint8_t profile_compat = sps[2];
    const uint8_t level_idc = sps[3];

    // AVCDecoderConfigurationRecord:
    // configurationVersion (1) | AVCProfileIndication (1) | profile_compatibility (1)
    // | AVCLevelIndication (1) | lengthSizeMinusOne (1, =0xFF for 4-byte NALU prefix)
    // | numSPS (1, =0xE1 for 1 SPS with 3 reserved bits set) | spsLen_hi | spsLen_lo | <SPS bytes>
    // | numPPS (1, =0x01) | ppsLen_hi | ppsLen_lo | <PPS bytes>
    out_avcc.clear();
    out_avcc.reserve(6 + 2 + sps_len + 1 + 2 + pps_len);
    out_avcc.push_back(0x01u);                                         // configurationVersion
    out_avcc.push_back(profile_idc);                                   // AVCProfileIndication
    out_avcc.push_back(profile_compat);                                // profile_compatibility
    out_avcc.push_back(level_idc);                                     // AVCLevelIndication
    out_avcc.push_back(0xFFu);                                         // lengthSizeMinusOne = 3 → 4-byte prefix
    out_avcc.push_back(0xE1u);                                         // numSPS = 1 (3 reserved + 5-bit count)
    out_avcc.push_back(static_cast<uint8_t>((sps_len >> 8u) & 0xFFu)); // spsLength high
    out_avcc.push_back(static_cast<uint8_t>(sps_len & 0xFFu));         // spsLength low
    out_avcc.insert(out_avcc.end(), sps, sps + sps_len);
    out_avcc.push_back(0x01u);                                         // numPPS = 1
    out_avcc.push_back(static_cast<uint8_t>((pps_len >> 8u) & 0xFFu)); // ppsLength high
    out_avcc.push_back(static_cast<uint8_t>(pps_len & 0xFFu));         // ppsLength low
    out_avcc.insert(out_avcc.end(), pps, pps + pps_len);

    return true;
}

bool ConvertAnnexBToAvcc(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (data == nullptr || size == 0)
        return false;

    std::vector<NalSpan> nals;
    EnumerateNals(data, size, nals);

    out.clear();
    for (const auto& nal : nals) {
        if (nal.nal_type == 9u) // AUD — excluded from sample payload
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
