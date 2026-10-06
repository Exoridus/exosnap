#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace exosnap::engine {

// Private BlockAdditionMapping. Standard block timestamps remain presentation
// times. This extension preserves exact backend timestamps for stream copying.
inline constexpr uint64_t kPacketTimestampBlockId = 2;
inline constexpr uint64_t kPacketTimestampMappingType = 0x45584454; // EXDT

struct MatroskaPacketTimestamps {
    uint64_t pts_ns;
    int64_t dts_ns;
};

inline std::array<uint8_t, 24> EncodeMatroskaPacketTimestamps(uint64_t pts_ns, int64_t dts_ns) noexcept {
    std::array<uint8_t, 24> bytes{'E', 'X', 'D', 'T', 1, 0, 0, 0};
    const uint64_t dts = std::bit_cast<uint64_t>(dts_ns);
    for (size_t i = 0; i < 8; ++i) {
        bytes[8 + i] = static_cast<uint8_t>(pts_ns >> (56 - 8 * i));
        bytes[16 + i] = static_cast<uint8_t>(dts >> (56 - 8 * i));
    }
    return bytes;
}

inline std::optional<MatroskaPacketTimestamps> DecodeMatroskaPacketTimestamps(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() != 24 || bytes[0] != 'E' || bytes[1] != 'X' || bytes[2] != 'D' || bytes[3] != 'T' ||
        bytes[4] != 1 || bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0)
        return std::nullopt;
    uint64_t pts = 0, dts = 0;
    for (size_t i = 0; i < 8; ++i) {
        pts = (pts << 8) | bytes[8 + i];
        dts = (dts << 8) | bytes[16 + i];
    }
    if (pts > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
        return std::nullopt;
    const auto signed_dts = std::bit_cast<int64_t>(dts);
    if (signed_dts == (std::numeric_limits<int64_t>::min)() || signed_dts > static_cast<int64_t>(pts))
        return std::nullopt;
    return MatroskaPacketTimestamps{pts, signed_dts};
}

} // namespace exosnap::engine
