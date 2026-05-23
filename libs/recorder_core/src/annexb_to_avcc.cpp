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

} // namespace recorder_core::annexb
