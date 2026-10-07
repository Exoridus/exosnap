#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace exosnap::engine {

struct EncodedVideoPacket {
    std::vector<uint8_t> bytes;
    uint64_t pts_ns = 0;
    bool keyframe = false;

    // Decode time orders compressed access units independently of presentation
    // time. Negative timestamps permit decoder preroll. Absent means decode
    // order equals presentation order. Producers emit access units in decode
    // order, including across calls and during flush.
    std::optional<int64_t> dts_ns;

    // Submit -> bitstream-available latency for this frame, in milliseconds.
    // Filled by the encoder when the bitstream is consumed (from the pending
    // frame's submit timestamp), so the true per-frame encode latency reaches
    // the diagnostics aggregator even when NVENC buffers a frame (P5-P7
    // NEED_MORE_INPUT: the consumed packet belongs to an earlier submission and
    // cannot be bracketed at the video-thread call site). Negative means the
    // latency is not available for this packet (e.g. a still-buffered frame, or a
    // non-NVENC producer); such packets are not reported to the aggregator.
    double encode_latency_ms = -1.0;

    // An output timestamp must identify a submitted picture. An unknown one is
    // fatal before packet creation. Keyframe prediction mismatches are warnings
    // only, because the encoded picture type owns random-access semantics.
    bool output_ts_mismatch = false;
    bool keyframe_prediction_mismatch = false;
};

struct EncodedAudioPacket {
    std::vector<uint8_t> bytes;
    uint64_t pts_ns = 0;
    uint32_t track_id = 0;
};

} // namespace exosnap::engine
