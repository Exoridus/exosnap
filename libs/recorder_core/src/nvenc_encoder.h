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
#include <recorder_core/color_metadata.h>

namespace recorder_core {

struct EncodedVideoPacket;

// NVENC error helpers
const char* NvencStatusName(NVENCSTATUS st) noexcept;

// ---------------------------------------------------------------------------
// ApplyColorMetadataToNvenc — pure, testable mapping from ColorMetadata to the
// NVENC bitstream-level color signaling fields (fix for color-range-signaling
// bug: without this the AV1/H.264/HEVC bitstream itself carries no color
// description, and — critically for AV1 — ffmpeg/most decoders derive
// color_range/matrix/primaries/transfer from the BITSTREAM, not the Matroska
// container Colour element, so an untagged bitstream shows up as
// color_range=tv (studio) + unknown matrix/primaries/transfer even though the
// container is tagged correctly. H.264/HEVC VUI use ITU-T Annex E flag/value
// semantics (videoFullRangeFlag: 0=limited/1=full); AV1's NV_ENC_CONFIG_AV1
// colorRange uses the same 0=studio/1=full convention. No GPU/NVENC session
// required — operates on a plain NV_ENC_CONFIG value.
// ---------------------------------------------------------------------------
void ApplyColorMetadataToNvenc(NV_ENC_CONFIG& cfg, VideoCodec codec, const ColorMetadata& color) noexcept;

// ---------------------------------------------------------------------------
// NvencPresetToGuid — pure, testable mapping from the canonical NvencPreset
// (P1..P7) to the NVENC SDK preset GUID. No GPU/NVENC session required.
// Applies uniformly across codecs — the caller passes the resulting GUID to
// nvEncGetEncodePresetConfigEx regardless of which codec GUID is also passed.
// ---------------------------------------------------------------------------
GUID NvencPresetToGuid(NvencPreset preset) noexcept;

// ---------------------------------------------------------------------------
// Chroma / input-format helpers — pure, testable, no GPU/NVENC session.
//
// NvencInputFormat: the NVENC input buffer format for a bit depth + chroma.
//   4:2:0  8-bit -> NV12 (semi-planar)
//   4:2:0 10-bit -> YUV420_10BIT (P010, semi-planar 16 bpc)
//   4:4:4  8-bit -> AYUV (packed A8Y8U8V8 — the DirectX 4:4:4 format NVENC
//                   consumes; planar YUV444 has no single D3D11 texture form).
//   4:4:4 10-bit is out of scope and never produced (blocked upstream).
//
// NvencChromaFormatIDC: the NV_ENC_CONFIG chromaFormatIDC value (1 = 4:2:0,
//   3 = 4:4:4) — see nvEncodeAPI.h H.264/HEVC config fields.
//
// Nvenc444ProfileGuid: the profile GUID enabling 4:4:4 for a codec — H.264
//   High 4:4:4 Predictive, HEVC Range Extensions (FREXT). Returns an all-zero
//   GUID for AV1 (NVENC AV1 is 4:2:0-only); callers must not enable 4:4:4 then.
// ---------------------------------------------------------------------------
NV_ENC_BUFFER_FORMAT NvencInputFormat(BitDepth depth, ChromaSubsampling chroma) noexcept;
uint32_t NvencChromaFormatIDC(ChromaSubsampling chroma) noexcept;
GUID Nvenc444ProfileGuid(VideoCodec codec) noexcept;

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

    // Set the chroma subsampling before calling Open()/FetchPresetConfig().
    // Defaults to Cs420. Cs444 selects AYUV input, chromaFormatIDC=3, and the
    // codec's 4:4:4 profile (H.264 High 4:4:4 / HEVC FREXT); it is valid only for
    // HevcNvenc and H264Nvenc at 8-bit (validated upstream — AV1 and 10-bit are
    // rejected before reaching the encoder).
    void SetChroma(ChromaSubsampling chroma) noexcept {
        m_chroma = chroma;
    }

    // Set quality tier before calling FetchPresetConfig(). Defaults to Balanced.
    // Only meaningful for ConstantQuality mode.
    void SetQualityPreset(NvencQualityPreset preset) noexcept {
        m_qualityPreset = preset;
    }

    // Set the NVENC speed/quality preset (P1..P7) before calling
    // FetchPresetConfig(). Defaults to P4. Applies uniformly for every codec —
    // see NvencPresetToGuid.
    void SetPreset(NvencPreset preset) noexcept {
        m_preset = preset;
    }

    // Set canonical rate-control mode and target bitrate (kbps).
    // Must be called before FetchPresetConfig(). Defaults: ConstantQuality / 20000.
    void SetRateControl(RateControlMode mode, uint32_t bitrate_kbps) noexcept {
        m_rateControlMode = mode;
        m_bitrate_kbps = bitrate_kbps;
    }

    // Set the color description that must be signaled in the encoded bitstream
    // (VUI for H.264/HEVC, NV_ENC_CONFIG_AV1 color fields for AV1). Must be
    // called before FetchPresetConfig(). Defaults to ColorMetadata::Sdr709().
    // This is the SAME ColorMetadata driving the VideoProcessor conversion
    // (video_thread.cpp) and the Matroska Colour element (matroska_stream_writer.cpp)
    // so all three writer paths agree — see color_metadata.h.
    void SetColor(const ColorMetadata& color) noexcept {
        m_color = color;
    }

    // Set keyframe interval in seconds. Must be called before InitEncoder().
    // Controls gopLength and idrPeriod: gopLength = round(secs * fps).
    // Default 2.0 s matches the pre-0.9.0 hardcoded value.
    void SetKeyframeIntervalSecs(float secs) noexcept {
        m_keyframeIntervalSecs = (secs > 0.0f) ? secs : 2.0f;
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

    // Honest 4:4:4 gate for the current codec (call after Open(), before
    // InitEncoder). Verifies the GPU advertises NV_ENC_CAPS_SUPPORT_YUV444_ENCODE
    // and that the AYUV input format is enumerated for the codec. Only meaningful
    // for H264Nvenc/HevcNvenc; fails honestly (out_error set) when 4:4:4 is
    // unavailable so the session can refuse rather than mis-encode.
    bool QueryYuv444Support(std::string& out_error);

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
    ChromaSubsampling m_chroma = ChromaSubsampling::Cs420;
    NvencQualityPreset m_qualityPreset = NvencQualityPreset::Balanced;
    RateControlMode m_rateControlMode = RateControlMode::ConstantQuality;
    uint32_t m_bitrate_kbps = 20000;
    ColorMetadata m_color = ColorMetadata::Sdr709();
    float m_keyframeIntervalSecs = 2.0f; // default 2 s — matches pre-0.9.0 hardcoded value

    // NVENC speed/quality preset (P1..P7), user-selectable expert setting.
    // Default P4 — matches the prior hardcoded AV1/HEVC default; H.264 previously
    // used P6 (visible default change, expert-overridable — see ADR 0039). P6 on
    // AV1/HEVC has internal pipeline depth that causes NV_ENC_ERR_NEED_MORE_INPUT
    // on every frame even with lookahead disabled; EncodeFrame already buffers/
    // drains this case via m_pendingPts/m_pendingSlots, so higher presets are not
    // fatal, but they increase encode latency and 8-slot input-ring pressure.
    NvencPreset m_preset = NvencPreset::P4;
    // Resolved via NvencPresetToGuid(m_preset) in FetchPresetConfig(); the member
    // initializer here is only the value before FetchPresetConfig() first runs.
    GUID m_presetGuid = NV_ENC_PRESET_P4_GUID;
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

    // In-band HDR10 metadata (HEVC SEI / AV1 metadata OBU) injected on every
    // keyframe. Built once by BuildHdrBitstreamPayloads() when the session is
    // HDR10-native; the payload byte buffers and the NVENC payload-descriptor
    // array are owned members so their pointers stay valid across the
    // synchronous NvEncEncodePicture call (NVENC reads them during that call).
    // Empty / count 0 for SDR and tone-map-SDR sessions, so their bitstream is
    // byte-identical to before this feature.
    std::vector<uint8_t> m_hdrMdcvPayload;
    std::vector<uint8_t> m_hdrCllPayload;
    std::array<NV_ENC_SEI_PAYLOAD, 2> m_hdrPayloadEntries{};
    uint32_t m_hdrPayloadCount = 0;
    // Deterministic IDR cadence tracking (gopLength; no B-frames / no lookahead,
    // so IDRs land on submission indices 0, gopLength, 2*gopLength, ... and each
    // forced IDR resets the phase). Used to attach HDR metadata on keyframes.
    uint32_t m_gopLength = 0;
    uint32_t m_frameInGop = 0;

    // Build the per-keyframe HDR metadata payloads for the current codec + color.
    // No-op (clears state) unless the session is HDR10-native on HEVC/AV1.
    void BuildHdrBitstreamPayloads();

    // Lock one bitstream and return an EncodedVideoPacket.
    // Also releases the associated input slot (unmap + mark free).
    bool LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error);
};

} // namespace recorder_core
