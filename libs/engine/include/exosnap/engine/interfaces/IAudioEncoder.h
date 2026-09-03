#pragma once
// Platform-agnostic audio encoder interface.
// Windows implementation: FfmpegAacEncoder.

#include <exosnap/engine/packet_types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::engine {

class IAudioEncoder {
  public:
    virtual ~IAudioEncoder() = default;

    // Initialize the encoder.
    virtual bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) = 0;

    // Feed interleaved float32 PCM samples.
    // accumulated_frames: in/out counter for PTS advancement across calls.
    // Appends any produced packets to out_packets.
    virtual void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns,
                             uint64_t& accumulated_frames, uint32_t sample_rate, uint32_t channels,
                             std::vector<EncodedAudioPacket>& out_packets) = 0;

    // Flush remaining output (EOS drain).
    virtual void Flush(std::vector<EncodedAudioPacket>& out_packets) = 0;

    // Codec-private bytes for the muxer (e.g. AudioSpecificConfig for AAC).
    // Valid after the first FeedFloat32() call that produces output.
    virtual std::vector<uint8_t> CodecPrivateBytes() const = 0;

    // Samples of encoder priming at the start of the stream, in the output
    // sample rate: what a decoder discards before the first real sample. The
    // container declares it (Matroska CodecDelay) and stores block timestamps
    // shifted by it, so the first audible sample lands on the timeline origin.
    // 0 for codecs without priming (PCM, FLAC).
    [[nodiscard]] virtual uint32_t CodecDelaySamples() const noexcept {
        return 0;
    }

    virtual void Shutdown() = 0;
};

} // namespace exosnap::engine
