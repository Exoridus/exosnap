#pragma once

#include "nvenc_encoder.h"
#include <exosnap/engine/interfaces/IVideoEncoder.h>

#include <string>
#include <vector>

namespace exosnap::engine {

// IVideoEncoder implementation wrapping NvencEncoder.
// Call SetCodec() and SetCq() before Open().
class NvencVideoEncoder : public IVideoEncoder {
  public:
    void SetCodec(VideoCodec codec) noexcept override {
        m_codec = codec;
        m_nvenc.SetCodec(codec);
    }

    // Set encoder bit depth (8 or 10) before Open()/Configure().
    void SetBitDepth(BitDepth depth) noexcept override {
        m_nvenc.SetBitDepth(depth);
    }

    // Set chroma subsampling before Open()/Configure(). Cs444 (8-bit H.264/HEVC)
    // selects AYUV input + the codec's 4:4:4 profile; defaults to Cs420.
    void SetChroma(ChromaSubsampling chroma) noexcept override {
        m_nvenc.SetChroma(chroma);
    }

    void SetCq(uint32_t cq) noexcept override {
        m_nvenc.SetCq(cq);
    }

    // Set the NVENC speed/quality preset (P1..P7) before Open()/Configure().
    // Defaults to P4. Applies uniformly for every codec — see NvencPresetToGuid.
    void SetPreset(NvencPreset preset) noexcept override {
        m_nvenc.SetPreset(preset);
    }

    // Set canonical rate-control mode and target bitrate before Configure().
    void SetRateControl(RateControlMode mode, uint32_t bitrate_kbps) noexcept override {
        m_nvenc.SetRateControl(mode, bitrate_kbps);
    }

    // Set the color description to signal in the encoded bitstream (VUI for
    // H.264/HEVC, AV1 color_config for AV1) before Configure(). See
    // NvencEncoder::SetColor.
    void SetColor(const ColorMetadata& color) noexcept override {
        m_nvenc.SetColor(color);
    }

    // Set keyframe interval in seconds before Configure().
    // Controls gopLength and idrPeriod: gopLength = round(secs * fps).
    // Default 2.0 s — matches the pre-0.9.0 hardcoded value.
    // Called from video_thread.cpp with the user's Settings → Advanced selection
    // before Configure(); the value flows into InitEncoder's GOP computation.
    void SetKeyframeIntervalSecs(float secs) noexcept override {
        m_nvenc.SetKeyframeIntervalSecs(secs);
    }

    // Declare the caller's submission regime before Configure(). Only affects the
    // hardware GOP backstop programmed into NVENC (see ComputeNvencGopBackstop);
    // the keyframe cadence is media-time based in both regimes.
    void SetConstantFrameRate(bool cfr) noexcept override {
        m_nvenc.SetConstantFrameRate(cfr);
    }

    // Resolved encoder init parameters, valid after Configure(). hdr_mode is left
    // at its default; the caller fills it from the session config.
    [[nodiscard]] EncoderInitInfo GetInitInfo() const noexcept override {
        return m_nvenc.GetInitInfo();
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
    void ReleaseSlot(int32_t slot_idx) noexcept override {
        m_nvenc.ReleaseSlot(slot_idx);
    }
    bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                     std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) override;
    bool ReapCompleted(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error,
                       uint32_t wait_head_ms = 0) override;
    bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) override;
    void RequestKeyframe() override {
        m_nvenc.RequestKeyframe();
    }
    void Destroy() override;

  private:
    NvencEncoder m_nvenc;
    VideoCodec m_codec = VideoCodec::Av1;
};

} // namespace exosnap::engine
