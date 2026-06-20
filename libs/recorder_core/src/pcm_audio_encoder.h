#pragma once

// PcmAudioEncoder: uncompressed PCM "encoder" (passthrough) for Matroska.
//
// PCM is not really encoded — it is a sample-format conversion. This class
// implements the same IAudioEncoder interface as the Opus/AAC encoders so the
// audio thread and mux path treat it identically, but instead of running a codec
// it converts the captured interleaved Float32 PCM into interleaved 16-bit
// signed little-endian (S16LE) samples and emits one packet per FeedFloat32
// call. The Matroska CodecID is A_PCM/INT_LIT with KaxAudioBitDepth=16 and no
// CodecPrivate (the format is fully described by the track header).
//
// Float→int16 conversion clamps to [-1, 1] and rounds to nearest, matching the
// behavior callers expect from the other encoders' float→PCM16 conversion.

#include <recorder_core/interfaces/IAudioEncoder.h>
#include <recorder_core/packet_types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recorder_core {

class PcmAudioEncoder : public IAudioEncoder {
  public:
    PcmAudioEncoder() = default;
    ~PcmAudioEncoder() override = default;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    // PCM has no buffered state to drain; this is a no-op.
    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    // A_PCM/INT_LIT carries no CodecPrivate — bit depth lives in the track header.
    std::vector<uint8_t> CodecPrivateBytes() const override;

    void Shutdown() override;

    // Convert one interleaved Float32 sample to S16LE, clamping to [-1, 1] and
    // rounding to nearest. Exposed static for unit testing.
    static int16_t Float32ToS16(float sample) noexcept;

    // The fixed sample format produced by this encoder.
    static constexpr uint32_t kBitDepth = 16;

  private:
    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
};

} // namespace recorder_core
