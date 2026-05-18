#pragma once

// NVENC AV1 encoder wrapper.
// All D3D11 context / video context usage is EXCLUSIVE to VideoThread.
// See D3D11 threading contract in video_thread.cpp.

#include <array>
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
// InputSlot — one NVENC GPU input resource in the slot ring
// ---------------------------------------------------------------------------

struct InputSlot {
    NV_ENC_REGISTERED_PTR registeredResource = nullptr;
    NV_ENC_INPUT_PTR mappedResource = nullptr;
    bool in_flight = false;
    bool mapped = false;
};

// ---------------------------------------------------------------------------
// NvencEncoder
// ---------------------------------------------------------------------------

class NvencEncoder {
  public:
    NvencEncoder() = default;
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    // Load nvEncodeAPI64.dll and open a D3D11 encode session.
    // device must remain valid for the lifetime of this encoder.
    bool Open(ID3D11Device* device, std::string& out_error);

    // Query AV1 GUID and NV12 format support.
    bool QueryAv1Nv12Support(std::string& out_error);

    // Fetch preset config and set chromaFormatIDC=1 (YUV420).
    bool FetchPresetConfig(std::string& out_error);

    // Initialize the encoder for the given dimensions and frame rate.
    bool InitEncoder(uint32_t width, uint32_t height, uint32_t frame_rate_num, uint32_t frame_rate_den,
                     std::string& out_error);

    // Create bitstream buffer.  Must be called after InitEncoder.
    bool CreateBitstreamBuffer(std::string& out_error);

    // Register one slot's NV12 D3D11 texture with NVENC.
    // Must be called after InitEncoder, once per slot (0..7).
    bool RegisterSlotTexture(int32_t slot_idx, ID3D11Texture2D* texture, std::string& out_error);

    // Acquire the next free input slot for writing.
    // Returns slot index (0–7) or -1 if none free.
    int32_t AcquireFreeSlot();

    // Submit one NV12 frame for encoding on a specific slot.
    // slot_idx must be a slot previously acquired via AcquireFreeSlot.
    // pts_ns is the capture-time PTS in nanoseconds.
    // Returns:
    //   true  + packet populated  -> output available immediately
    //   true  + packet empty      -> NV_ENC_ERR_NEED_MORE_INPUT (buffered, PTS queued)
    //   false                     -> fatal encode error (out_error set)
    bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height, EncodedVideoPacket* out_packet,
                     std::string& out_error);

    // Flush all buffered frames (EOS drain).
    // Appends any remaining packets to out_packets.
    bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error);

    // Unregister all slot resources.  Safe to call multiple times.
    void UnregisterAllSlots();

    // Destroy bitstream buffer and encoder session.
    void Destroy();

  private:
    HMODULE m_dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_funcs{};
    void* m_encoder = nullptr;
    NV_ENC_PRESET_CONFIG m_presetConfig{};
    NV_ENC_CONFIG m_encodeConfig{};
    NV_ENC_OUTPUT_PTR m_bitstreamBuffer = nullptr;

    // Input-slot ring: 8 independent NV12 input resources
    std::array<InputSlot, 8> m_slots;
    int32_t m_slotCursor = 0;

    const GUID m_presetGuid = NV_ENC_PRESET_P4_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    // Pending PTS FIFO — one entry per submitted frame not yet returned as output
    std::queue<uint64_t> m_pendingPts;

    // Pending slot FIFO — mirrors m_pendingPts; associates slot indices with pending output
    std::queue<int32_t> m_pendingSlots;

    int m_needMoreInputCount = 0;

    // Per-instance monotonic frame index for NVENC inputTimeStamp
    uint64_t m_frameIdx = 0;

    // Lock one bitstream and return an EncodedVideoPacket.
    // Also releases the associated input slot (unmap + mark free).
    bool LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error);
};

} // namespace recorder_core
