#include "nvenc_video_encoder.h"
#include <d3d11.h>

namespace recorder_core {

bool NvencVideoEncoder::Open(void* gpu_context, std::string& out_error) {
    auto* device = static_cast<ID3D11Device*>(gpu_context);
    if (!m_nvenc.Open(device, out_error))
        return false;
    bool ok = false;
    if (m_codec == VideoCodec::H264Nvenc) {
        ok = m_nvenc.QueryH264Nv12Support(out_error);
    } else if (m_codec == VideoCodec::HevcNvenc) {
        ok = m_nvenc.QueryHevcNv12Support(out_error);
    } else {
        ok = m_nvenc.QueryAv1Nv12Support(out_error);
    }
    return ok;
}

bool NvencVideoEncoder::Configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                                  std::string& out_error) {
    if (!m_nvenc.FetchPresetConfig(out_error))
        return false;
    if (!m_nvenc.InitEncoder(width, height, fps_num, fps_den, out_error))
        return false;
    if (!m_nvenc.CreateBitstreamBuffer(out_error))
        return false;
    return true;
}

bool NvencVideoEncoder::RegisterSlotTexture(int32_t slot_idx, GpuTextureHandle texture, std::string& out_error) {
    return m_nvenc.RegisterSlotTexture(slot_idx, static_cast<ID3D11Texture2D*>(texture), out_error);
}

bool NvencVideoEncoder::EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                                    EncodedVideoPacket& out_packet, std::string& out_error) {
    return m_nvenc.EncodeFrame(slot_idx, pts_ns, width, height, &out_packet, out_error);
}

bool NvencVideoEncoder::Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) {
    return m_nvenc.Flush(out_packets, out_error);
}

void NvencVideoEncoder::Destroy() {
    m_nvenc.UnregisterAllSlots();
    m_nvenc.Destroy();
}

} // namespace recorder_core
