#pragma once

// NVENC encoder wrapper (AV1 and H.264).
// All D3D11 context / video context usage is EXCLUSIVE to VideoThread.
// See D3D11 threading contract in video_thread.cpp.

#include <array>
#include <chrono>
#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include <d3d11.h>
#include <windows.h>

#include "nvEncodeAPI.h"

#include <recorder_core/codec_types.h>
#include <recorder_core/color_metadata.h>
#include <recorder_core/pipeline_diagnostics.h>

namespace recorder_core {

struct EncodedVideoPacket;

// NVENC error helpers
const char* NvencStatusName(NVENCSTATUS st) noexcept;

// ---------------------------------------------------------------------------
// Bounded flush-drain policy — pure, testable. The shutdown flush drains the
// encoder's buffered frames with a non-blocking lock (doNotWait=1) and consults
// this after each attempt. Guarantees the drain always terminates: on a lost or
// hung device the lock stays busy forever, so once the time budget is exceeded
// the drain aborts and the caller finalises anyway (no join-timeout wedge). No
// GPU/NVENC session required.
// ---------------------------------------------------------------------------
enum class FlushDrainStep {
    Consume,      // NV_ENC_SUCCESS: a packet is ready — take it and continue draining.
    Retry,        // NV_ENC_ERR_LOCK_BUSY within budget — brief wait, then poll again.
    AbortTimeout, // LOCK_BUSY past the budget — device not delivering: stop the drain.
    AbortError,   // Any other status — stop the drain.
};
FlushDrainStep NextFlushDrainStep(NVENCSTATUS lock_status, double elapsed_ms, double budget_ms) noexcept;

// ---------------------------------------------------------------------------
// Bounded event-drain policy — pure, testable. Same shape as
// FlushDrainStep/NextFlushDrainStep, generalised from an NVENCSTATUS lock
// result to a Win32 WaitForSingleObject result: the async-mode submit path
// (waiting for a free output slot's completion event) and the async-mode
// flush drain (waiting for the remaining pending frames' completion events)
// both consult this after every wait. Same anti-wedge guarantee as the sync
// flush drain: a device that never signals (Device-Lost) must not hang
// forever — past the budget the wait aborts and the caller proceeds. No
// GPU/NVENC session required.
// ---------------------------------------------------------------------------
enum class EventDrainStep {
    Consume,      // WAIT_OBJECT_0: the event fired — a packet is ready, take it.
    Retry,        // WAIT_TIMEOUT within budget — brief wait, then poll again.
    AbortTimeout, // WAIT_TIMEOUT past the budget — device not signalling: stop.
    AbortError,   // Any other result (WAIT_FAILED, WAIT_ABANDONED, ...) — stop.
};
EventDrainStep NextEventDrainStep(DWORD wait_result, double elapsed_ms, double budget_ms) noexcept;

// ---------------------------------------------------------------------------
// FindFreeOutputSlot — pure, testable round-robin scan for a free async
// output-ring slot. Same round-robin-from-cursor pattern as
// AcquireFreeSlot (member function, mutates m_slots for the 8-slot input
// ring), generalised into a pure function over an explicit in-flight array so
// the output ring's free/in-flight bookkeeping is unit-testable without a
// live NVENC session. "Oldest in-flight" needs no separate helper: with
// frameIntervalP=1 (no B-frames/lookahead) output order == submission order,
// so the oldest is always the PendingFrame FIFO head. No GPU/NVENC session.
// ---------------------------------------------------------------------------
struct FreeOutputSlotResult {
    int32_t slot_idx = -1;   // -1 if every slot in [0, count) is in-flight
    int32_t next_cursor = 0; // cursor to pass on the next call
};
FreeOutputSlotResult FindFreeOutputSlot(const bool* in_flight, int32_t count, int32_t cursor) noexcept;

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
RcParams ComputeNvencRcParams(RateControlMode mode, uint32_t cq, uint32_t bitrate_kbps);

// ---------------------------------------------------------------------------
// GOP / keyframe helpers — pure, testable, no GPU/NVENC session.
//
// ComputeGopLength: gopLength = round(keyframe_interval_secs * fps), fps =
//   num/den. Falls back to 120 (the historical 2 s @ 60 fps default) for a
//   degenerate frame rate, and never returns 0 (an all-1-GOP infinite stream).
//
// ApplyGopToNvenc: writes gopLength and the codec-specific idrPeriod into an
//   NV_ENC_CONFIG, keeping idrPeriod == gopLength for H.264/HEVC/AV1 (each codec
//   config struct carries its own idrPeriod field).
// ---------------------------------------------------------------------------
uint32_t ComputeGopLength(float keyframe_interval_secs, uint32_t frame_rate_num, uint32_t frame_rate_den) noexcept;
void ApplyGopToNvenc(NV_ENC_CONFIG& cfg, VideoCodec codec, uint32_t gop_length) noexcept;

// ---------------------------------------------------------------------------
// ApplySpatialAqToNvenc — pure, testable. Explicitly pins spatial adaptive
// quantization on so the AQ state is set by us, not inherited from the driver's
// per-preset default. Spatial AQ (rcParams.enableAQ) has no capability gate in
// the NVENC API, unlike temporal AQ (NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ), and is
// valid with the P-only / no-lookahead pipeline used here. Temporal AQ is left
// off deliberately: nvEncodeAPI.h does not document it as valid without
// lookahead, so enabling it would be speculative. aqStrength stays 0 to keep the
// driver's automatic strength selection (header: "If not set, strength is auto
// selected by driver."). No GPU/NVENC session required.
// ---------------------------------------------------------------------------
void ApplySpatialAqToNvenc(NV_ENC_CONFIG& cfg) noexcept;

// ---------------------------------------------------------------------------
// NextGopKeyframePhase — pure, testable IDR cadence decision. EncodeFrame does
// not merely predict IDR placement from NVENC's idrPeriod timer — it actively
// sets NV_ENC_PIC_FLAG_FORCEIDR on every submission this function marks as a
// keyframe, so cadence is an enforced fact rather than an assumption about
// driver behavior (idrPeriod stays set as a belt-and-braces backstop only).
// With no B-frames and no lookahead (frameIntervalP=1) output order ==
// submission order, so IDRs land on submission indices 0, gopLength,
// 2*gopLength, ...; a forced IDR resets the phase. Given the current
// frame-in-GOP counter, the configured GOP length, and whether a forced IDR
// was requested this frame, returns whether this frame is a keyframe and the
// counter to carry to the next frame. No GPU/NVENC session.
// ---------------------------------------------------------------------------
struct GopKeyframePhase {
    bool is_keyframe = false;
    uint32_t frame_in_gop = 0;
};
GopKeyframePhase NextGopKeyframePhase(uint32_t frame_in_gop, uint32_t gop_length, bool forced_idr) noexcept;

// ---------------------------------------------------------------------------
// ResyncGopPhaseFromActual — pure order/keyframe hardening (warn-first).
// NextGopKeyframePhase predicts IDR placement at submission time; the actual
// pictureType observed when a bitstream is consumed is the ground truth. A
// real IDR always restarts the GOP regardless of what was predicted for that
// submission (self-healing: any drift accumulated under buffered presets is
// corrected the next time an actual IDR is observed). A non-IDR output leaves
// the counter untouched — the submission side is already advancing it
// independently, and a single non-keyframe mismatch is not evidence the whole
// cadence has shifted. No GPU/NVENC session.
// ---------------------------------------------------------------------------
uint32_t ResyncGopPhaseFromActual(bool actual_is_idr, uint32_t frame_in_gop) noexcept;

// ---------------------------------------------------------------------------
// Pure message formatters for the encoder's two output-order validations.
// Kept pure so the exact wording is unit-testable without a GPU/NVENC
// session.
//
// FormatOutputTsMismatchError: a timestamp-echo mismatch is fatal — the call
// site logs it once and aborts the encode, so no once-per-session guard is
// needed (it cannot recur within a session).
// FormatKeyframePredictionMismatchWarning stays warn-only (a predicted
// keyframe landing on a non-IDR frame is legal, just off-cadence SEI/OBU
// placement); its call site still guards it behind a once-per-session flag
// so a sustained mismatch does not spam the log.
// ---------------------------------------------------------------------------
std::string FormatOutputTsMismatchError(uint64_t expected_output_ts, uint64_t actual_output_ts);
std::string FormatKeyframePredictionMismatchWarning(bool predicted_keyframe, bool actual_keyframe);

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
    void SetCq(uint32_t cq) noexcept {
        m_cq = cq;
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

    // Resolved encoder initialization parameters, valid after a successful
    // InitEncoder() (i.e. after Configure()). Plain data for diagnostics / the
    // session report — carries no NVENC types. hdr_mode is not known here (it is a
    // pipeline-level concept); the caller fills it from the session config.
    [[nodiscard]] EncoderInitInfo GetInitInfo() const noexcept {
        EncoderInitInfo info;
        info.valid = m_gopLength > 0; // set by InitEncoder
        info.codec = m_codec;
        info.preset = m_preset;
        info.rc_mode = m_rateControlMode;
        info.target_bitrate_kbps = m_encodeConfig.rcParams.averageBitRate / 1000;
        info.max_bitrate_kbps = m_encodeConfig.rcParams.maxBitRate / 1000;
        info.cq = m_cq;
        info.gop_length = m_gopLength;
        info.bframes = m_encodeConfig.frameIntervalP > 0 ? m_encodeConfig.frameIntervalP - 1 : 0;
        info.lookahead_frames = m_encodeConfig.rcParams.enableLookahead ? m_encodeConfig.rcParams.lookaheadDepth : 0;
        info.temporal_aq = m_encodeConfig.rcParams.enableTemporalAQ != 0;
        info.spatial_aq = m_encodeConfig.rcParams.enableAQ != 0;
        info.bit_depth = m_bitDepth;
        info.chroma = m_chroma;
        info.color_full_range = (m_color.range == ColorRange::Full);
        return info;
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
    // Appends 0..k completed packets to out_packets: 0..1 in sync mode. An
    // async submission always appends 0 of its own output here — that arrives
    // later via ReapCompleted — but MAY append one older packet as a side
    // effect of a bounded wait for a free output-ring slot.
    // Returns false only on a fatal encode error (out_error set).
    bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                     std::vector<EncodedVideoPacket>& out_packets, std::string& out_error);

    // Drain packets completed since the last EncodeFrame/ReapCompleted call
    // (async mode only — a no-op returning true in sync mode, since sync
    // output is always consumed inline by EncodeFrame). Waits up to
    // wait_head_ms for the oldest pending frame's completion event; once that
    // one is consumed (or immediately, if wait_head_ms is 0 and nothing is yet
    // ready), drains any further already-signalled packets without additional
    // waiting.
    bool ReapCompleted(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error, uint32_t wait_head_ms = 0);

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
    // Sync-mode single output buffer (unchanged — the sync path remains fully
    // intact as the capability fallback). Unused when m_asyncMode is true.
    NV_ENC_OUTPUT_PTR m_bitstreamBuffer = nullptr;

    // Async-mode output ring. kMaxOutputResources is the allocation ceiling
    // (covers the observed P6/P7 pipeline depth, plus headroom for a future
    // higher-depth pipeline once lookahead/B-frames need one);
    // m_activeDepth (1..kMaxOutputResources) is how many of them are
    // actually used by Submit/Reap this session. Ship default is 1: a pure
    // correctness fix with no extra VRAM use vs. sync mode — raising it is a
    // deliberately deferred follow-up (would need an expert setting + a VRAM
    // clamp, not part of this step). Events are registered for all
    // kMaxOutputResources slots regardless of m_activeDepth (cheap; avoids
    // re-registration if the depth ever becomes configurable at Configure()
    // time).
    static constexpr int32_t kMaxOutputResources = 4;
    struct OutputResource {
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        HANDLE event = nullptr;
        bool in_flight = false;
    };
    std::array<OutputResource, kMaxOutputResources> m_outputResources{};
    int32_t m_activeDepth = 1;
    int32_t m_outputCursor = 0;
    bool m_asyncMode = false;
    // Async mode only: EOS submissions carry no output buffer, but the SDK's
    // async contract still requires a valid completionEvent on every
    // NvEncEncodePicture call — this one is reserved for that purpose.
    HANDLE m_eosEvent = nullptr;

    // Input-slot ring: 8 independent NV12 input resources
    std::array<InputSlot, 8> m_slots;
    int32_t m_slotCursor = 0;

    VideoCodec m_codec = VideoCodec::Av1Nvenc;
    BitDepth m_bitDepth = BitDepth::Bit8;
    ChromaSubsampling m_chroma = ChromaSubsampling::Cs420;
    uint32_t m_cq = CanonicalCq(NvencQualityPreset::Balanced);
    RateControlMode m_rateControlMode = RateControlMode::ConstantQuality;
    uint32_t m_bitrate_kbps = 20000;
    ColorMetadata m_color = ColorMetadata::Sdr709();
    float m_keyframeIntervalSecs = 2.0f; // default 2 s — matches pre-0.9.0 hardcoded value

    // NVENC speed/quality preset (P1..P7), user-selectable expert setting.
    // Default P4 — matches the prior hardcoded AV1/HEVC default; H.264 previously
    // used P6 (visible default change, expert-overridable — see ADR 0039). P6 on
    // AV1/HEVC has internal pipeline depth that causes NV_ENC_ERR_NEED_MORE_INPUT
    // on every frame even with lookahead disabled; EncodeFrame already buffers/
    // drains this case via the m_pending FIFO, so higher presets are not
    // fatal, but they increase encode latency and 8-slot input-ring pressure.
    NvencPreset m_preset = NvencPreset::P4;
    // Resolved via NvencPresetToGuid(m_preset) in FetchPresetConfig(); the member
    // initializer here is only the value before FetchPresetConfig() first runs.
    GUID m_presetGuid = NV_ENC_PRESET_P4_GUID;
    const NV_ENC_TUNING_INFO m_tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    // One entry per submitted frame not yet returned as output. Consolidates the
    // former parallel PTS/slot FIFOs into a single record so the submit timestamp
    // travels with each in-flight frame; the consuming lock computes the true
    // submit->ready latency from it (carried out on EncodedVideoPacket::
    // encode_latency_ms). Output order == submission order today (frameIntervalP=1,
    // no lookahead), so front() is always the next output — behaviour-identical to
    // the previous two-FIFO scheme.
    struct PendingFrame {
        uint64_t pts_ns = 0;
        int32_t slot_idx = -1;
        std::chrono::steady_clock::time_point submit_time{};
        // Order-validation fields: the inputTimeStamp submitted for this frame
        // (compared against lockBS.outputTimeStamp on consume — a mismatch is
        // fatal, see LockAndConsumeBitstream) and the submission-side keyframe
        // prediction (compared against the actual lockBS.pictureType, warn-only).
        uint64_t input_ts = 0;
        bool predicted_keyframe = false;
        // Which output-ring slot this submission's bitstream/event lives in
        // (async mode only; -1/unused in sync mode, which has a single shared
        // m_bitstreamBuffer and no completion event).
        int32_t out_idx = -1;
    };
    std::queue<PendingFrame> m_pending;

    int m_needMoreInputCount = 0;

    // Once-per-session log guard for the keyframe-prediction mismatch, which
    // stays warn-only (a predicted keyframe landing on a non-IDR frame is
    // legal, just off-cadence SEI/OBU placement). The outputTimeStamp mismatch
    // has no guard: it is fatal, so it can by construction never log more than
    // once (the encode aborts on the first occurrence). The cumulative
    // counter for the keyframe case lives in the diagnostics aggregator
    // (EncoderDiagnostics::keyframe_prediction_mismatches), fed per-packet
    // like encode_latency_ms — NvencEncoder has no aggregator reference.
    // Reset in InitEncoder.
    bool m_loggedKeyframePredictionMismatch = false;

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
    // Lock and consume one buffered output frame. non_blocking sets doNotWait=1
    // so a not-yet-ready output returns NV_ENC_ERR_LOCK_BUSY immediately (nothing
    // is consumed — safe to retry) instead of blocking; out_lock_status, when
    // provided, receives the raw nvEncLockBitstream status for the drain policy.
    bool LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error, bool non_blocking = false,
                                 NVENCSTATUS* out_lock_status = nullptr);

    // Async mode only: bounded wait on one specific completion event, then
    // lock+consume via LockAndConsumeBitstream (non-blocking — the event being
    // signalled already guarantees the output is ready). Retries on the shared
    // NextEventDrainStep policy until Consume, or the budget/an error aborts
    // the wait. Shared by EncodeFrame's output-ring-full wait and Flush's
    // async drain loop, which both need the same bounded-wait behavior.
    EventDrainStep WaitAndConsumeOneAsync(HANDLE event, double budget_ms, EncodedVideoPacket& out_packet,
                                          std::string& out_error);

    // Async-mode Flush: submits EOS on its reserved event, then drains all
    // remaining PendingFrames on the same bounded NextEventDrainStep policy as
    // WaitAndConsumeOneAsync (2000 ms budget per progress, matching the
    // existing sync flush drain's anti-wedge guarantee).
    bool FlushAsync(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error);

    // Tear down the async output ring — every event unregistered + closed
    // first, then every bitstream buffer destroyed. Safe to call multiple
    // times and on partial state (rollback from a failed
    // CreateBitstreamBuffer, or normal Destroy()).
    void DestroyOutputRing() noexcept;
};

} // namespace recorder_core
