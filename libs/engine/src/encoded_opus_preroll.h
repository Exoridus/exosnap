#pragma once

#include <exosnap/engine/packet_types.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace exosnap::engine {

struct OpusPrerollSelection {
    size_t first_index = 0;
    size_t one_past_last_index = 0;
    uint64_t origin_r_ns = 0;
    uint64_t audible_start_r_ns = 0;
    uint32_t decoder_discard_samples = 0;
    uint64_t container_delay_ns = 0;
};

// Input packet times are already mapped to recording nanoseconds for one track.
// The returned packets precede the first packet heard in the new segment.
[[nodiscard]] inline std::optional<OpusPrerollSelection> SelectOpusPreroll(std::span<const EncodedAudioPacket> history,
                                                                           uint32_t track_id,
                                                                           uint64_t first_audible_packet_r_ns,
                                                                           uint32_t frame_samples) noexcept {
    constexpr uint32_t kRequiredSamples = 3840;
    constexpr uint32_t kSampleRate = 48000;
    if (frame_samples != 120 && frame_samples != 240 && frame_samples != 480 && frame_samples != 960)
        return std::nullopt;
    const uint64_t packet_duration_ns = static_cast<uint64_t>(frame_samples) * 1000000000ULL / kSampleRate;
    const size_t required_packets = (kRequiredSamples + frame_samples - 1) / frame_samples;
    if (history.size() < required_packets ||
        required_packets * static_cast<size_t>(frame_samples) > (std::numeric_limits<uint16_t>::max)())
        return std::nullopt;

    uint64_t expected_end = first_audible_packet_r_ns;
    const size_t first = history.size() - required_packets;
    for (size_t index = history.size(); index > first; --index) {
        const auto& packet = history[index - 1];
        if (packet.track_id != track_id || packet.bytes.empty() || packet.pts_ns > expected_end ||
            expected_end - packet.pts_ns != packet_duration_ns)
            return std::nullopt;
        expected_end = packet.pts_ns;
    }
    const auto discard_samples = static_cast<uint32_t>(required_packets * frame_samples);
    const auto delay_ns = static_cast<uint64_t>(discard_samples) * 1000000000ULL / kSampleRate;
    return OpusPrerollSelection{first,           history.size(), expected_end, first_audible_packet_r_ns,
                                discard_samples, delay_ns};
}

} // namespace exosnap::engine
