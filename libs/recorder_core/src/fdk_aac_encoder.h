#pragma once

#include <recorder_core/interfaces/IAudioEncoder.h>

#include <cstdint>
#include <string>
#include <vector>

struct AACENCODER;

namespace recorder_core {

class FdkAacEncoder : public IAudioEncoder {
  public:
    ~FdkAacEncoder() override;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    std::vector<uint8_t> CodecPrivateBytes() const override;

    void Shutdown() override;

  private:
    static constexpr int kFrameSizeSamples = 1024;

    AACENCODER* m_enc = nullptr;
    uint32_t m_sample_rate = 0;
    uint32_t m_channels = 0;
    uint64_t m_accumulated_frames = 0; // running PCM frame count for flush PTS
    std::vector<uint8_t> m_codec_private;
    std::vector<int16_t> m_frame_buf;

    void DrainEncoder(std::vector<EncodedAudioPacket>& out_packets);
};

} // namespace recorder_core
