#include "codec_random_access.h"
#include "codec_private.h"
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace exosnap::engine {
namespace {

bool QualifyLengthPrefixed(std::span<const uint8_t> packet, unsigned length_bytes, bool hevc) noexcept {
    if (length_bytes == 0 || length_bytes > 4 || packet.empty())
        return false;
    bool saw_idr = false;
    size_t offset = 0;
    while (offset < packet.size()) {
        if (packet.size() - offset < length_bytes)
            return false;
        size_t length = 0;
        for (unsigned index = 0; index < length_bytes; ++index)
            length = (length << 8) | packet[offset++];
        if (length == 0 || length > packet.size() - offset)
            return false;
        const auto nal = packet.subspan(offset, length);
        if (nal[0] & 0x80)
            return false;
        if (hevc) {
            if (nal.size() < 3 || (nal[1] & 0x07) == 0)
                return false;
            const unsigned type = (nal[0] >> 1) & 0x3f;
            if (type <= 31) {
                if (type != 19 && type != 20)
                    return false;
                saw_idr = true;
            }
        } else {
            if (nal.size() < 2)
                return false;
            const unsigned type = nal[0] & 0x1f;
            if (type >= 1 && type <= 5) {
                if (type != 5)
                    return false;
                saw_idr = true;
            }
        }
        offset += length;
    }
    return saw_idr;
}

bool ReadObuLength(std::span<const uint8_t> bytes, size_t& cursor, size_t& length) noexcept {
    length = 0;
    for (unsigned index = 0; index < 8; ++index) {
        if (cursor == bytes.size())
            return false;
        const uint8_t byte = bytes[cursor++];
        if (index == 7 && (byte & 0x80))
            return false;
        if (static_cast<uint64_t>(byte & 0x7f) > ((std::numeric_limits<size_t>::max)() >> (index * 7)))
            return false;
        length |= static_cast<size_t>(byte & 0x7f) << (index * 7);
        if ((byte & 0x80) == 0)
            return true;
    }
    return false;
}

bool QualifyAv1(std::span<const uint8_t> codec_private, std::span<const uint8_t> packet) noexcept {
    if (codec_private.size() < 4 || codec_private[0] != 0x81)
        return false;
    const unsigned configured_profile = (codec_private[1] >> 5) & 0x07;
    if (configured_profile > 2)
        return false;
    bool sequence_seen = false;
    bool frame_seen = false;
    bool reduced_still_picture = false;
    size_t offset = 0;
    while (offset < packet.size()) {
        const uint8_t header = packet[offset++];
        if ((header & 0x81) != 0)
            return false;
        const unsigned type = (header >> 3) & 0x0f;
        if (header & 0x04) {
            if (offset == packet.size())
                return false;
            const uint8_t extension = packet[offset++];
            if ((extension & 0x07) != 0)
                return false;
        }
        if ((header & 0x02) == 0)
            return false;
        size_t length = 0;
        if (!ReadObuLength(packet, offset, length) || length > packet.size() - offset)
            return false;
        const auto payload = packet.subspan(offset, length);
        offset += length;
        if (type == 1) {
            if (sequence_seen || frame_seen || payload.empty())
                return false;
            const unsigned profile = payload[0] >> 5;
            const bool still_picture = (payload[0] & 0x10) != 0;
            reduced_still_picture = (payload[0] & 0x08) != 0;
            if (profile != configured_profile || (reduced_still_picture && !still_picture))
                return false;
            sequence_seen = true;
        } else if (type == 3 || type == 6) {
            if (!sequence_seen || frame_seen || payload.size() < 2)
                return false;
            frame_seen = true;
            if (!reduced_still_picture) {
                const bool show_existing = (payload[0] & 0x80) != 0;
                const unsigned frame_type = (payload[0] >> 5) & 0x03;
                const bool show_frame = (payload[0] & 0x10) != 0;
                if (show_existing || frame_type != 0 || !show_frame)
                    return false;
            }
        }
    }
    if (!sequence_seen || !frame_seen)
        return false;
    try {
        std::vector<uint8_t> derived;
        char reason[128]{};
        if (!codec_private::DeriveAv1CodecPrivate(packet.data(), packet.size(), derived, reason, sizeof(reason)))
            return false;
        return derived.size() == codec_private.size() &&
               std::equal(derived.begin(), derived.end(), codec_private.begin());
    } catch (...) {
        return false;
    }
}

} // namespace

RandomAccessKind QualifyClosedRandomAccess(std::string_view codec_id, std::span<const uint8_t> codec_private,
                                           std::span<const uint8_t> packet, bool advertised_key) noexcept {
    if (!advertised_key || packet.empty())
        return RandomAccessKind::Unknown;
    if (codec_id == "V_MPEG4/ISO/AVC" && codec_private.size() >= 5 && codec_private[0] == 1 &&
        QualifyLengthPrefixed(packet, (codec_private[4] & 0x03) + 1, false))
        return RandomAccessKind::ClosedGop;
    if (codec_id == "V_MPEGH/ISO/HEVC" && codec_private.size() >= 22 && codec_private[0] == 1 &&
        QualifyLengthPrefixed(packet, (codec_private[21] & 0x03) + 1, true))
        return RandomAccessKind::ClosedGop;
    if (codec_id == "V_AV1" && QualifyAv1(codec_private, packet))
        return RandomAccessKind::ClosedGop;
    return RandomAccessKind::Unknown;
}

} // namespace exosnap::engine
