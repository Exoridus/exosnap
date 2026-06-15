#pragma once

#include "nvenc_encoder.h"
#include <recorder_core/interfaces/IVideoEncoder.h>

#include <string>
#include <vector>

namespace recorder_core {

// IVideoEncoder implementation wrapping NvencEncoder.
// Call SetCodec() and SetQualityPreset() before Open().
class NvencVideoEncoder : public IVideoEncoder {
  public:
    void SetCodec(VideoCodec codec) noexcept {
        m_codec = codec;
        m_nvenc.SetCodec(codec);
    }

    void SetQualityPreset(NvencQualityPreset preset) noexcept {
        m_nvenc.SetQualityPreset(preset);
    }

    // Set canonical rate-control mode and target bitrate before Configure().
    void SetRateControl(RateControlMode mode, uint32_t bitrate_kbps) noexcept {
        m_nvenc.SetRateControl(mode, bitrate_kbps);
    }

    bool Open(void* gpu_context, std::string& out_error) override;
    bool Configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                   std::string& out_error) override;
    bool RegisterSlotTexture(int32_t slot_idx, GpuTextureHandle texture, std::string& out_error) override;
    int32_t SlotCount() const override {
        return 8;
    }
    int32_t AcquireFreeSlot() override {
        return m_nvenc.AcquireFreeSlot();
    }
    bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height, EncodedVideoPacket& out_packet,
                     std::string& out_error) override;
    bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) override;
    void RequestKeyframe() override {
        m_nvenc.RequestKeyframe();
    }
    void Destroy() override;

    // Expose ReleaseSlot for error-path callers that acquired a slot but did not submit it.
    void ReleaseSlot(int32_t slot_idx) noexcept {
        m_nvenc.ReleaseSlot(slot_idx);
    }

  private:
    NvencEncoder m_nvenc;
    VideoCodec m_codec = VideoCodec::Av1Nvenc;
};

} // namespace recorder_core
