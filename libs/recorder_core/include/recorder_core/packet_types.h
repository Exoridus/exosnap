#pragma once

#include <cstdint>
#include <vector>

namespace recorder_core {

struct EncodedVideoPacket {
    std::vector<uint8_t> bytes;
    uint64_t pts_ns = 0;
    bool keyframe = false;

    // Submit -> bitstream-available latency for this frame, in milliseconds.
    // Filled by the encoder when the bitstream is consumed (from the pending
    // frame's submit timestamp), so the true per-frame encode latency reaches
    // the diagnostics aggregator even when NVENC buffers a frame (P5-P7
    // NEED_MORE_INPUT: the consumed packet belongs to an earlier submission and
    // cannot be bracketed at the video-thread call site). Negative means the
    // latency is not available for this packet (e.g. a still-buffered frame, or a
    // non-NVENC producer); such packets are not reported to the aggregator.
    double encode_latency_ms = -1.0;
};

struct EncodedAudioPacket {
    std::vector<uint8_t> bytes;
    uint64_t pts_ns = 0;
    uint32_t track_id = 0;
};

} // namespace recorder_core
