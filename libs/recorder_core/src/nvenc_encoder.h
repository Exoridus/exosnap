#pragma once

// NVENC AV1 encoder wrapper.
// All D3D11 context / video context usage is EXCLUSIVE to VideoThread.
// See D3D11 threading contract in video_thread.cpp.

#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include <d3d11.h>
#include <windows.h>

#include "nvEncodeAPI.h"

namespace recorder_core {

struct EncodedVideoPacket;

// NVENC error helpers
const char* NvencStatusName(NVENCSTATUS st) noexcept;

// ---------------------------------------------------------------------------
// NvencEncoder
// ---------------------------------------------------------------------------

class NvencEncoder {
public:
    NvencEncoder() = default;
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&)            = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    // Load nvEncodeAPI64.dll and open a D3D11 encode session.
    // device must remain valid for the lifetime of this encoder.
    bool Open(ID3D11Device* device, std::string& out_error);

    // Query AV1 GUID and NV12 format support.
    bool QueryAv1Nv12Support(std::string& out_error);

    // Fetch preset config and set chromaFormatIDC=1 (YUV420).
    bool FetchPresetConfig(std::string& out_error);

    // Initialize the encoder for the given dimensions and frame rate.
    bool InitEncoder(
        uint32_t width, uint32_t height,
        uint32_t frame_rate_num, uint32_t frame_rate_den,
        std::string& out_error);

    // Create bitstream buffer.  Must be called after InitEncoder.
    bool CreateBitstreamBuffer(std::string& out_error);

    // Register a D3D11 NV12 texture with NVENC.
    // Must be called after InitEncoder.
    bool RegisterNv12Texture(ID3D11Texture2D* texture, std::string& out_error);

    // Unregister the previously registered resource.
    bool UnregisterResource(std::string& out_error);

    // Submit one NV12 frame for encoding.
    // pts_ns is the capture-time PTS in nanoseconds.
    // Returns:
    //   true  + packet populated  -> output available immediately
    //   true  + packet empty      -> NV_ENC_ERR_NEED_MORE_INPUT (buffered, PTS queued)
    //   false                     -> fatal encode error (out_error set)
    bool EncodeFrame(
        uint64_t                 pts_ns,
        uint32_t                 width,
        uint32_t                 height,
        EncodedVideoPacket*      out_packet,
        std::string&             out_error);

    // Flush all buffered frames (EOS drain).
    // Appends any remaining packets to out_packets.
    bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error);

    // Destroy bitstream buffer and encoder session.
    void Destroy();

private:
    HMODULE                     m_dll             = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_funcs{};
    void*                       m_encoder         = nullptr;
    NV_ENC_PRESET_CONFIG        m_presetConfig{};
    NV_ENC_CONFIG               m_encodeConfig{};
    NV_ENC_OUTPUT_PTR           m_bitstreamBuffer  = nullptr;
    NV_ENC_REGISTERED_PTR       m_registeredResource = nullptr;

    const GUID               m_presetGuid  = NV_ENC_PRESET_P4_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo  = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    // Pending PTS FIFO — one entry per submitted frame not yet returned as output
    std::queue<uint64_t> m_pendingPts;
    int                  m_needMoreInputCount = 0;

    // Per-instance monotonic frame index for NVENC inputTimeStamp
    uint64_t             m_frameIdx           = 0;

    // Lock one bitstream and return an EncodedVideoPacket.
    bool LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error);
};

} // namespace recorder_core
