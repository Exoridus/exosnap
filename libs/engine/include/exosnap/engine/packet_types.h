#pragma once

#include <cstdint>
#include <vector>

namespace exosnap::engine {

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

    // Order/keyframe validation results for this packet: true when the
    // driver's actual outputTimeStamp / pictureType disagreed with the
    // submission-side FIFO assignment / GOP-phase prediction for this frame.
    // A keyframe-prediction mismatch is warn-only. An outputTimeStamp
    // mismatch is fatal and aborts the encode before a packet is built, so
    // output_ts_mismatch is never true on any packet that actually reaches
    // this struct — it stays here for API symmetry with
    // keyframe_prediction_mismatch. Filled by the encoder at consume time;
    // the video thread reports these to the diagnostics aggregator (same
    // per-packet transport as encode_latency_ms — the encoder itself has no
    // aggregator reference).
    bool output_ts_mismatch = false;
    bool keyframe_prediction_mismatch = false;
};

struct EncodedAudioPacket {
    std::vector<uint8_t> bytes;
    uint64_t pts_ns = 0;
    uint32_t track_id = 0;
};

} // namespace exosnap::engine
