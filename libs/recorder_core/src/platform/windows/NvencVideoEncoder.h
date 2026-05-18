#pragma once
// Windows adapter: wraps NvencEncoder behind IVideoEncoder.
// Full implementation wiring (session thread integration) is deferred to a later phase.
//
// D3D11 threading contract: all methods must be called from the VideoThread exclusively.

#include <recorder_core/interfaces/IVideoEncoder.h>

#include "../../nvenc_encoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace recorder_core {

class NvencVideoEncoder : public IVideoEncoder {
  public:
    NvencVideoEncoder();
    ~NvencVideoEncoder() override;

    NvencVideoEncoder(const NvencVideoEncoder&) = delete;
    NvencVideoEncoder& operator=(const NvencVideoEncoder&) = delete;

    // gpu_context: ID3D11Device* cast to void*.
    bool Open(void* gpu_context, std::string& out_error) override;

    bool Configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                   std::string& out_error) override;

    // texture: ID3D11Texture2D* cast to void*.
    bool RegisterSlotTexture(int32_t slot_idx, GpuTextureHandle texture, std::string& out_error) override;

    int32_t SlotCount() const override;

    int32_t AcquireFreeSlot() override;

    bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height, EncodedVideoPacket& out_packet,
                     std::string& out_error) override;

    bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) override;

    void Destroy() override;

  private:
    NvencEncoder m_encoder;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace recorder_core
