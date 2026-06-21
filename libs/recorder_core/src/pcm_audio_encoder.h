#pragma once

// PcmAudioEncoder: uncompressed PCM "encoder" (passthrough) for Matroska.
//
// PCM is not really encoded — it is a sample-format conversion. This class
// implements the same IAudioEncoder interface as the Opus/AAC encoders so the
// audio thread and mux path treat it identically, but instead of running a codec
// it converts the captured interleaved Float32 PCM into interleaved signed
// little-endian samples at the configured bit depth (16, 24, or 32 bits) and
// emits one packet per FeedFloat32 call. The Matroska CodecID is A_PCM/INT_LIT
// with KaxAudioBitDepth set to the configured depth; there is no CodecPrivate
// (the format is fully described by the track header).
//
// Float→int conversion clamps to [-1, 1] and rounds to nearest. The bit depth
// must be set via SetBitDepth() before Init(); the default is 16.

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

    // Set the output bit depth before Init(). Accepted values: 16, 24, 32.
    // 16 is the default (backward-compatible). Must be called before Init().
    void SetBitDepth(uint32_t bits) noexcept;

    // Return the effective bit depth (as configured, or 16 if never set).
    [[nodiscard]] uint32_t BitDepth() const noexcept {
        return m_bit_depth;
    }

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

    // Convert one interleaved Float32 sample to a packed S24LE triple (3 bytes,
    // little-endian). dst must point to at least 3 writable bytes.
    static void Float32ToS24LE(float sample, uint8_t* dst) noexcept;

    // Convert one interleaved Float32 sample to S32LE (4 bytes, little-endian).
    static int32_t Float32ToS32(float sample) noexcept;

    // The default bit depth (backward-compatible).
    static constexpr uint32_t kBitDepth = 16;

  private:
    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bit_depth = kBitDepth;
};

} // namespace recorder_core
