#pragma once

// NVENC encoder wrapper (AV1 and H.264).
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

#include <recorder_core/codec_types.h>

namespace recorder_core {

struct EncodedVideoPacket;

// NVENC error helpers
const char* NvencStatusName(NVENCSTATUS st) noexcept;

// ---------------------------------------------------------------------------
// RcParams — pure value type for NVENC rate-control parameters.
// Used by ComputeNvencRcParams (testable without GPU).
// ---------------------------------------------------------------------------

struct RcParams {
    // NV_ENC_PARAMS_RC_MODE value — NV_ENC_PARAMS_RC_CONSTQP, _VBR, or _CBR
    uint32_t rateControlMode = 0;
    // constQP fields (valid for ConstantQuality; zero otherwise)
    uint32_t qpIntra = 0;
    uint32_t qpInterP = 0;
    uint32_t qpInterB = 0;
    // Bitrate fields (NV_ENC_RC_PARAMS::averageBitRate / maxBitRate, in bps)
    uint32_t averageBitRate = 0;
    uint32_t maxBitRate = 0;
};

// Pure, testable mapping from canonical rate-control mode to NVENC parameters.
// No GPU or NVENC session required. Used by FetchPresetConfig().
// NVENC SDK field names: rcParams.rateControlMode, rcParams.averageBitRate,
//   rcParams.maxBitRate, rcParams.constQP.{qpIntra, qpInterP, qpInterB}.
RcParams ComputeNvencRcParams(RateControlMode mode, NvencQualityPreset quality, uint32_t bitrate_kbps);

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

    // Set codec before calling Open(). Defaults to Av1Nvenc.
    void SetCodec(VideoCodec codec) noexcept {
        m_codec = codec;
    }

    // Set encoder bit depth before calling Open()/FetchPresetConfig(). Defaults to Bit8.
    // Bit10 selects P010 (NV_ENC_BUFFER_FORMAT_YUV420_10BIT) input and the HEVC Main10 /
    // AV1 10-bit profile. Only valid for HevcNvenc and Av1Nvenc (validated upstream).
    void SetBitDepth(BitDepth depth) noexcept {
        m_bitDepth = depth;
    }

    // Set quality tier before calling FetchPresetConfig(). Defaults to Balanced.
    // Only meaningful for ConstantQuality mode.
    void SetQualityPreset(NvencQualityPreset preset) noexcept {
        m_qualityPreset = preset;
    }

    // Set canonical rate-control mode and target bitrate (kbps).
    // Must be called before FetchPresetConfig(). Defaults: ConstantQuality / 20000.
    void SetRateControl(RateControlMode mode, uint32_t bitrate_kbps) noexcept {
        m_rateControlMode = mode;
        m_bitrate_kbps = bitrate_kbps;
    }

    // Load nvEncodeAPI64.dll and open a D3D11 encode session.
    // device must remain valid for the lifetime of this encoder.
    bool Open(ID3D11Device* device, std::string& out_error);

    // Query AV1 GUID and NV12 format support.
    bool QueryAv1Nv12Support(std::string& out_error);

    // Query H.264 GUID and NV12 format support.
    bool QueryH264Nv12Support(std::string& out_error);

    // Query HEVC (H.265) GUID and NV12 format support.
    bool QueryHevcNv12Support(std::string& out_error);

    // Fetch preset config and set chromaFormatIDC=1 (YUV420).
    bool FetchPresetConfig(std::string& out_error);

    // Initialize the encoder for the given dimensions and frame rate.
    bool InitEncoder(uint32_t width, uint32_t height, uint32_t frame_rate_num, uint32_t frame_rate_den,
                     std::string& out_error);

    // Create bitstream buffer.  Must be called after InitEncoder.
    bool CreateBitstreamBuffer(std::string& out_error);

    // Register one slot's D3D11 texture with NVENC (NV12 for 8-bit, P010 for 10-bit).
    // Must be called after InitEncoder, once per slot (0..7).
    bool RegisterSlotTexture(int32_t slot_idx, ID3D11Texture2D* texture, std::string& out_error);

    // Acquire the next free input slot for writing.
    // Returns slot index (0–7) or -1 if none free.
    int32_t AcquireFreeSlot();

    // Release a slot that was acquired but not submitted (error path only).
    // Clears in_flight without calling NVENC — safe only if EncodeFrame was not called.
    void ReleaseSlot(int32_t slot_idx) noexcept;

    // Arm a forced IDR (keyframe) on the NEXT submitted frame. Used at a segment
    // boundary so the first frame of a new segment is a self-contained keyframe
    // with fresh SPS/PPS (no dependent frame precedes it). One-shot: the flag is
    // consumed by the next EncodeFrame call.
    void RequestKeyframe() noexcept {
        m_forceIdrNext = true;
    }

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

    VideoCodec m_codec = VideoCodec::Av1Nvenc;
    BitDepth m_bitDepth = BitDepth::Bit8;
    NvencQualityPreset m_qualityPreset = NvencQualityPreset::Balanced;
    RateControlMode m_rateControlMode = RateControlMode::ConstantQuality;
    uint32_t m_bitrate_kbps = 20000;

    // P6 for H.264 (synchronous), P4 for AV1 (P6 AV1 has internal pipeline depth
    // that causes NV_ENC_ERR_NEED_MORE_INPUT on every frame even with lookahead disabled).
    GUID m_presetGuid = NV_ENC_PRESET_P6_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    // Pending PTS FIFO — one entry per submitted frame not yet returned as output
    std::queue<uint64_t> m_pendingPts;

    // Pending slot FIFO — mirrors m_pendingPts; associates slot indices with pending output
    std::queue<int32_t> m_pendingSlots;

    int m_needMoreInputCount = 0;

    // Per-instance monotonic frame index for NVENC inputTimeStamp
    uint64_t m_frameIdx = 0;

    // One-shot forced-IDR request consumed by the next EncodeFrame submission.
    bool m_forceIdrNext = false;

    // Lock one bitstream and return an EncodedVideoPacket.
    // Also releases the associated input slot (unmap + mark free).
    bool LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error);
};

} // namespace recorder_core
