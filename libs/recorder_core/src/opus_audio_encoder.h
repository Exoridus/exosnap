#pragma once

#include <recorder_core/interfaces/IAudioEncoder.h>

#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;

namespace recorder_core {

class OpusAudioEncoder : public IAudioEncoder {
  public:
    ~OpusAudioEncoder() override;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    // Mid-stream discontinuity: reset encoder state and discard partial frame buffer.
    // Returns the number of audio frames that were discarded (to advance PTS).
    uint64_t ResetState();

    std::vector<uint8_t> CodecPrivateBytes() const override;

    uint64_t EmittedFrames() const {
        return m_emitted_frames;
    }

    void Shutdown() override;

  private:
    static constexpr int kFrameSizeSamples = 960; // 20ms @ 48kHz

    ::OpusEncoder* m_encoder = nullptr;
    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    std::vector<uint8_t> m_codec_private;
    std::vector<float> m_frame_buffer;
    uint64_t m_emitted_frames = 0;
};

} // namespace recorder_core
