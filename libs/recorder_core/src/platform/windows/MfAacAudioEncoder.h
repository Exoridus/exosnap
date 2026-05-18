#pragma once
// Windows adapter: wraps MfAacEncoder behind IAudioEncoder.
// Full implementation wiring (session thread integration) is deferred to a later phase.

#include <recorder_core/interfaces/IAudioEncoder.h>

#include "../../mf_aac_encoder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recorder_core {

class MfAacAudioEncoder : public IAudioEncoder {
  public:
    MfAacAudioEncoder();
    ~MfAacAudioEncoder() override;

    MfAacAudioEncoder(const MfAacAudioEncoder&) = delete;
    MfAacAudioEncoder& operator=(const MfAacAudioEncoder&) = delete;

    bool Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) override;

    void FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns, uint64_t& accumulated_frames,
                     uint32_t sample_rate, uint32_t channels, std::vector<EncodedAudioPacket>& out_packets) override;

    void Flush(std::vector<EncodedAudioPacket>& out_packets) override;

    // Returns the AudioSpecificConfig blob. Valid after the first FeedFloat32()
    // call that produces output.
    std::vector<uint8_t> CodecPrivateBytes() const override;

    void Shutdown() override;

  private:
    MfAacEncoder m_encoder;
};

} // namespace recorder_core
