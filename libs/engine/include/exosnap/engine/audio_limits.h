#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace exosnap::engine {

inline constexpr uint32_t kMaxAudioTracks = 8;
inline constexpr uint32_t kMaxAudioSources = 16;
inline constexpr uint32_t kMaxPhysicalAudioCaptures = 16;
inline constexpr uint32_t kMaxAudioRoutes = 128;
inline constexpr uint64_t kAudioHoldHorizonNs = 3'000'000'000ULL;
inline constexpr size_t kAudioPremuxPayloadBytes = 32ULL * 1024 * 1024;
inline constexpr size_t kMuxReorderPayloadBytes = 64ULL * 1024 * 1024;
inline constexpr uint64_t kMinimumAudioPacketDurationNs = 2'500'000;

[[nodiscard]] constexpr size_t AudioHoldPacketLimit(uint32_t tracks) noexcept {
    return static_cast<size_t>((std::min)(tracks, kMaxAudioTracks)) *
           static_cast<size_t>(kAudioHoldHorizonNs / kMinimumAudioPacketDurationNs);
}

[[nodiscard]] constexpr size_t ReorderPacketLimit(uint32_t tracks, uint32_t video_fps,
                                                  uint64_t audio_packet_duration_ns,
                                                  size_t decoder_reorder_allowance = 32) noexcept {
    const uint64_t duration = (std::max)(audio_packet_duration_ns, kMinimumAudioPacketDurationNs);
    const uint64_t rate =
        static_cast<uint64_t>((std::min)(tracks, kMaxAudioTracks)) * ((1'000'000'000ULL + duration - 1) / duration) +
        video_fps;
    return (std::max)(size_t{4096}, static_cast<size_t>(3 * rate + (rate + 3) / 4) + decoder_reorder_allowance);
}

} // namespace exosnap::engine
