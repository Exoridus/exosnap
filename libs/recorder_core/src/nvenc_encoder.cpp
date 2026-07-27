#include "nvenc_encoder.h"

#include <recorder_core/hdr_bitstream_metadata.h>
#include <recorder_core/logging/logging.h>
#include <recorder_core/packet_types.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace recorder_core {

namespace {

std::string GuidDebugString(const GUID& guid) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
             guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

const char* TuningInfoName(NV_ENC_TUNING_INFO info) noexcept {
    switch (info) {
    case NV_ENC_TUNING_INFO_UNDEFINED:
        return "NV_ENC_TUNING_INFO_UNDEFINED";
    case NV_ENC_TUNING_INFO_HIGH_QUALITY:
        return "NV_ENC_TUNING_INFO_HIGH_QUALITY";
    case NV_ENC_TUNING_INFO_LOW_LATENCY:
        return "NV_ENC_TUNING_INFO_LOW_LATENCY";
    case NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY:
        return "NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY";
    case NV_ENC_TUNING_INFO_LOSSLESS:
        return "NV_ENC_TUNING_INFO_LOSSLESS";
    case NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY:
        return "NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY";
    default:
        return "NV_ENC_TUNING_INFO_UNKNOWN";
    }
}

bool QueryEncodeCap(NV_ENCODE_API_FUNCTION_LIST& funcs, void* encoder, GUID codecGuid, NV_ENC_CAPS cap, int& out_value,
                    std::string& out_error) {
    if (funcs.nvEncGetEncodeCaps == nullptr) {
        out_error = "nvEncGetEncodeCaps is unavailable";
        return false;
    }

    NV_ENC_CAPS_PARAM capsParam{};
    capsParam.version = NV_ENC_CAPS_PARAM_VER;
    capsParam.capsToQuery = cap;

    int value = 0;
    const NVENCSTATUS st = funcs.nvEncGetEncodeCaps(encoder, codecGuid, &capsParam, &value);
    if (st != NV_ENC_SUCCESS) {
        out_error =
            std::string("nvEncGetEncodeCaps(") + std::to_string(static_cast<int>(cap)) + "): " + NvencStatusName(st);
        return false;
    }

    out_value = value;
    return true;
}

static const char* BufferFormatName(NV_ENC_BUFFER_FORMAT fmt) noexcept {
    switch (fmt) {
    case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
        return "NV_ENC_BUFFER_FORMAT_YUV420_10BIT";
    case NV_ENC_BUFFER_FORMAT_AYUV:
        return "NV_ENC_BUFFER_FORMAT_AYUV";
    default:
        return "NV_ENC_BUFFER_FORMAT_NV12";
    }
}

// Codec label for init diagnostics (0=AV1, 1=H264, 2=HEVC)
static const char* CodecLabel(int codec_index) noexcept {
    if (codec_index == 1)
        return "H264";
    if (codec_index == 2)
        return "HEVC";
    return "AV1";
}

std::string BuildInitDiagString(const NV_ENC_INITIALIZE_PARAMS& p, const NV_ENC_CONFIG& cfg, int codec_index,
                                bool have_caps, int w_min, int w_max, int h_min, int h_max, BitDepth bit_depth,
                                ChromaSubsampling chroma) {
    const bool isH264 = (codec_index == 1);
    const bool isHevc = (codec_index == 2);
    std::ostringstream oss;
    oss << "encodeGUID=" << GuidDebugString(p.encodeGUID) << ", presetGUID=" << GuidDebugString(p.presetGUID)
        << ", profileGUID=" << GuidDebugString(cfg.profileGUID) << ", tuningInfo=" << TuningInfoName(p.tuningInfo)
        << ", encodeWidth=" << p.encodeWidth << ", encodeHeight=" << p.encodeHeight << ", darWidth=" << p.darWidth
        << ", darHeight=" << p.darHeight << ", maxEncodeWidth=" << p.maxEncodeWidth
        << ", maxEncodeHeight=" << p.maxEncodeHeight << ", frameRateNum=" << p.frameRateNum
        << ", frameRateDen=" << p.frameRateDen
        << ", bufferFormat=" << BufferFormatName(NvencInputFormat(bit_depth, chroma))
        << ", enablePTD=" << static_cast<int>(p.enablePTD)
        << ", enableEncodeAsync=" << static_cast<int>(p.enableEncodeAsync)
        << ", rateControlMode=" << static_cast<uint32_t>(cfg.rcParams.rateControlMode)
        << ", gopLength=" << cfg.gopLength << ", frameIntervalP=" << cfg.frameIntervalP;

    if (isH264) {
        const auto& h264 = cfg.encodeCodecConfig.h264Config;
        oss << ", h264.idrPeriod=" << h264.idrPeriod
            << ", h264.chromaFormatIDC=" << static_cast<unsigned>(h264.chromaFormatIDC);
    } else if (isHevc) {
        const auto& hevc = cfg.encodeCodecConfig.hevcConfig;
        oss << ", hevc.idrPeriod=" << hevc.idrPeriod
            << ", hevc.chromaFormatIDC=" << static_cast<unsigned>(hevc.chromaFormatIDC) << ", hevc.level=" << hevc.level
            << ", hevc.tier=" << hevc.tier;
    } else {
        const auto& av1 = cfg.encodeCodecConfig.av1Config;
        oss << ", av1.idrPeriod=" << av1.idrPeriod
            << ", av1.chromaFormatIDC=" << static_cast<unsigned>(av1.chromaFormatIDC) << ", av1.level=" << av1.level
            << ", av1.tier=" << av1.tier;
    }

    const char* label = CodecLabel(codec_index);
    const std::string capsPrefix = std::string(", ") + label + "Caps.";
    if (have_caps) {
        oss << capsPrefix << "widthMin=" << w_min << capsPrefix << "widthMax=" << w_max << capsPrefix
            << "heightMin=" << h_min << capsPrefix << "heightMax=" << h_max;
    } else {
        oss << ", " << label << "Caps=unavailable";
    }

    return oss.str();
}

} // namespace

// ---------------------------------------------------------------------------
// ApplyColorMetadataToNvenc
// ---------------------------------------------------------------------------
//
// Root cause of the color-range-signaling bug (measured on a real AV1+Opus+MKV
// recording): NVENC's H.264/HEVC VUI and AV1 color_config fields were never
// populated, so the bitstream itself carried no color description. This is
// benign for H.264/HEVC in Matroska (ffmpeg's demuxer falls back to the
// container Colour element when the bitstream is untagged — verified
// empirically), but AV1 is NOT: ffmpeg's AV1 parser derives color_range /
// color_space / color_transfer / color_primaries EXCLUSIVELY from the AV1
// sequence header's color_config, ignoring the container tag entirely even
// when it is present and correct. An untagged AV1 bitstream reproduces the
// exact measured symptom: color_range=tv (studio) and matrix/primaries/transfer
// = unknown, regardless of what the Matroska Colour element says. Populating
// these fields from the same ColorMetadata that drives the VideoProcessor
// conversion and the container tags closes the gap for all three codecs.
void ApplyColorMetadataToNvenc(NV_ENC_CONFIG& cfg, VideoCodec codec, const ColorMetadata& color) noexcept {
    // Matches the fullRange rule already used for the VideoProcessor output
    // color space in video_thread.cpp: only ColorRange::Limited is treated as
    // studio range; Unspecified defaults to full (never produced by
    // translation.cpp today, but kept consistent defensively).
    const bool fullRange = (color.range != ColorRange::Limited);
    const auto primaries = static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(color.primaries);
    const auto transfer = static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(color.transfer);
    const auto matrix = static_cast<NV_ENC_VUI_MATRIX_COEFFS>(color.matrix);

    if (codec == VideoCodec::Av1) {
        auto& av1 = cfg.encodeCodecConfig.av1Config;
        av1.colorPrimaries = primaries;
        av1.transferCharacteristics = transfer;
        av1.matrixCoefficients = matrix;
        // AV1 colorRange convention: 0 = studio swing, 1 = full swing.
        av1.colorRange = fullRange ? 1u : 0u;
        return;
    }

    // H.264 and HEVC share the identical VUI parameters layout
    // (NV_ENC_CONFIG_HEVC_VUI_PARAMETERS is a typedef of the H.264 struct).
    NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui = (codec == VideoCodec::Hevc)
                                                 ? cfg.encodeCodecConfig.hevcConfig.hevcVUIParameters
                                                 : cfg.encodeCodecConfig.h264Config.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    // ITU-T Annex E videoFullRangeFlag convention: 0 = limited, 1 = full.
    vui.videoFullRangeFlag = fullRange ? 1u : 0u;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = primaries;
    vui.transferCharacteristics = transfer;
    vui.colourMatrix = matrix;
}

// ---------------------------------------------------------------------------
// NvencPresetToGuid — pure mapping from the canonical NvencPreset to the
// NVENC SDK preset GUID (NV_ENC_PRESET_P1_GUID .. NV_ENC_PRESET_P7_GUID).
// Applies uniformly across codecs; the caller passes the resulting GUID to
// nvEncGetEncodePresetConfigEx together with whichever codec GUID is active.
// ---------------------------------------------------------------------------
GUID NvencPresetToGuid(NvencPreset preset) noexcept {
    switch (preset) {
    case NvencPreset::P1:
        return NV_ENC_PRESET_P1_GUID;
    case NvencPreset::P2:
        return NV_ENC_PRESET_P2_GUID;
    case NvencPreset::P3:
        return NV_ENC_PRESET_P3_GUID;
    case NvencPreset::P4:
        return NV_ENC_PRESET_P4_GUID;
    case NvencPreset::P5:
        return NV_ENC_PRESET_P5_GUID;
    case NvencPreset::P6:
        return NV_ENC_PRESET_P6_GUID;
    case NvencPreset::P7:
        return NV_ENC_PRESET_P7_GUID;
    }
    return NV_ENC_PRESET_P4_GUID;
}

// ---------------------------------------------------------------------------
// Chroma / input-format helpers (pure, testable — see nvenc_encoder.h)
// ---------------------------------------------------------------------------

NV_ENC_BUFFER_FORMAT NvencInputFormat(BitDepth depth, ChromaSubsampling chroma) noexcept {
    if (chroma == ChromaSubsampling::Cs444) {
        // 8-bit 4:4:4 only; 10-bit 4:4:4 is out of scope and blocked upstream.
        return NV_ENC_BUFFER_FORMAT_AYUV;
    }
    return (depth == BitDepth::Bit10) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
}

uint32_t NvencChromaFormatIDC(ChromaSubsampling chroma) noexcept {
    return (chroma == ChromaSubsampling::Cs444) ? 3u : 1u;
}

GUID Nvenc444ProfileGuid(VideoCodec codec) noexcept {
    if (codec == VideoCodec::H264) {
        return NV_ENC_H264_PROFILE_HIGH_444_GUID;
    }
    if (codec == VideoCodec::Hevc) {
        // HEVC Range Extensions (FREXT) — the 4:2:2/4:4:4 8/10-bit profile.
        return NV_ENC_HEVC_PROFILE_FREXT_GUID;
    }
    // AV1 NVENC has no 4:4:4 profile.
    return GUID{};
}

// ---------------------------------------------------------------------------
// NvencStatusName
// ---------------------------------------------------------------------------

const char* NvencStatusName(NVENCSTATUS st) noexcept {
    switch (st) {
    case NV_ENC_SUCCESS:
        return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
        return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
        return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:
        return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
        return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR:
        return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT:
        return "NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM:
        return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:
        return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:
        return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
        return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
        return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY:
        return "NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:
        return "NV_ENC_ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION:
        return "NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED:
        return "NV_ENC_ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT:
        return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:
        return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:
        return "NV_ENC_ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC:
        return "NV_ENC_ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
        return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:
        return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
        return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:
        return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:
        return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
    default:
        return "NV_ENC_ERR_UNKNOWN";
    }
}

FlushDrainStep NextFlushDrainStep(NVENCSTATUS lock_status, double elapsed_ms, double budget_ms) noexcept {
    switch (lock_status) {
    case NV_ENC_SUCCESS:
        return FlushDrainStep::Consume;
    case NV_ENC_ERR_LOCK_BUSY:
        // Output not ready yet: keep polling until the budget runs out, then give
        // up so a lost/hung device can never wedge the drain.
        return elapsed_ms < budget_ms ? FlushDrainStep::Retry : FlushDrainStep::AbortTimeout;
    default:
        return FlushDrainStep::AbortError;
    }
}

EventDrainStep NextEventDrainStep(DWORD wait_result, double elapsed_ms, double budget_ms) noexcept {
    switch (wait_result) {
    case WAIT_OBJECT_0:
        return EventDrainStep::Consume;
    case WAIT_TIMEOUT:
        // Event not signalled yet: keep polling until the budget runs out, then
        // give up so a lost/hung device (event never fires) can never wedge.
        return elapsed_ms < budget_ms ? EventDrainStep::Retry : EventDrainStep::AbortTimeout;
    default:
        return EventDrainStep::AbortError;
    }
}

FreeOutputSlotResult FindFreeOutputSlot(const bool* in_flight, int32_t count, int32_t cursor) noexcept {
    FreeOutputSlotResult result;
    result.next_cursor = cursor;
    if (count <= 0)
        return result;
    for (int32_t i = 0; i < count; ++i) {
        const int32_t idx = (cursor + i) % count;
        if (!in_flight[idx]) {
            result.slot_idx = idx;
            result.next_cursor = (idx + 1) % count;
            return result;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

NvencEncoder::~NvencEncoder() {
    Destroy();
    if (m_dll) {
        FreeLibrary(m_dll);
        m_dll = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

bool NvencEncoder::Open(ID3D11Device* device, std::string& out_error) {
    m_dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_dll) {
        char buf[80];
        snprintf(buf, sizeof(buf), "LoadLibraryW(nvEncodeAPI64.dll) failed, GetLastError=%lu",
                 static_cast<unsigned long>(GetLastError()));
        out_error = buf;
        return false;
    }

    using PFN = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<PFN>(GetProcAddress(m_dll, "NvEncodeAPICreateInstance"));
    if (!pCreate) {
        out_error = "NvEncodeAPICreateInstance not exported from nvEncodeAPI64.dll";
        return false;
    }

    m_funcs = {};
    m_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = pCreate(&m_funcs);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("NvEncodeAPICreateInstance: ") + NvencStatusName(st);
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device;
    params.apiVersion = NVENCAPI_VERSION;

    st = m_funcs.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS || !m_encoder) {
        out_error = std::string("nvEncOpenEncodeSessionEx: ") + NvencStatusName(st);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// QueryAv1Nv12Support
// ---------------------------------------------------------------------------

bool NvencEncoder::QueryAv1Nv12Support(std::string& out_error) {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetEncodeGUIDCount(m_encoder, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetEncodeGUIDCount: ") + NvencStatusName(st);
        return false;
    }

    std::vector<GUID> guids(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetEncodeGUIDs(m_encoder, guids.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetEncodeGUIDs: ") + NvencStatusName(st);
        return false;
    }

    bool av1Found = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (IsEqualGUID(guids[i], NV_ENC_CODEC_AV1_GUID) != 0) {
            av1Found = true;
            break;
        }
    }
    if (!av1Found) {
        out_error = "NV_ENC_CODEC_AV1_GUID not found (Ada Lovelace / RTX 40+ required)";
        return false;
    }

    count = 0;
    st = m_funcs.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_AV1_GUID, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetInputFormatCount: ") + NvencStatusName(st);
        return false;
    }

    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    st = m_funcs.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_AV1_GUID, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetInputFormats: ") + NvencStatusName(st);
        return false;
    }

    const NV_ENC_BUFFER_FORMAT wantFmt = NvencInputFormat(m_bitDepth, m_chroma);
    bool fmtFound = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == wantFmt) {
            fmtFound = true;
            break;
        }
    }
    if (!fmtFound) {
        out_error = std::string(BufferFormatName(wantFmt)) + " not in AV1 input formats";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// QueryH264Nv12Support
// ---------------------------------------------------------------------------

bool NvencEncoder::QueryH264Nv12Support(std::string& out_error) {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetEncodeGUIDCount(m_encoder, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetEncodeGUIDCount: ") + NvencStatusName(st);
        return false;
    }

    std::vector<GUID> guids(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetEncodeGUIDs(m_encoder, guids.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetEncodeGUIDs: ") + NvencStatusName(st);
        return false;
    }

    bool h264Found = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (IsEqualGUID(guids[i], NV_ENC_CODEC_H264_GUID) != 0) {
            h264Found = true;
            break;
        }
    }
    if (!h264Found) {
        out_error = "NV_ENC_CODEC_H264_GUID not found";
        return false;
    }

    count = 0;
    st = m_funcs.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_H264_GUID, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetInputFormatCount(H264): ") + NvencStatusName(st);
        return false;
    }

    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    st = m_funcs.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_H264_GUID, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetInputFormats(H264): ") + NvencStatusName(st);
        return false;
    }

    bool nv12Found = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_NV12) {
            nv12Found = true;
            break;
        }
    }
    if (!nv12Found) {
        out_error = "NV_ENC_BUFFER_FORMAT_NV12 not in H264 input formats";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// QueryHevcNv12Support
// ---------------------------------------------------------------------------

bool NvencEncoder::QueryHevcNv12Support(std::string& out_error) {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetEncodeGUIDCount(m_encoder, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetEncodeGUIDCount: ") + NvencStatusName(st);
        return false;
    }

    std::vector<GUID> guids(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetEncodeGUIDs(m_encoder, guids.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetEncodeGUIDs: ") + NvencStatusName(st);
        return false;
    }

    bool hevcFound = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (IsEqualGUID(guids[i], NV_ENC_CODEC_HEVC_GUID) != 0) {
            hevcFound = true;
            break;
        }
    }
    if (!hevcFound) {
        out_error = "NV_ENC_CODEC_HEVC_GUID not found (Kepler / GTX 600+ required)";
        return false;
    }

    count = 0;
    st = m_funcs.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_HEVC_GUID, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetInputFormatCount(HEVC): ") + NvencStatusName(st);
        return false;
    }

    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    st = m_funcs.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_HEVC_GUID, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetInputFormats(HEVC): ") + NvencStatusName(st);
        return false;
    }

    const NV_ENC_BUFFER_FORMAT wantFmt = NvencInputFormat(m_bitDepth, m_chroma);
    bool fmtFound = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == wantFmt) {
            fmtFound = true;
            break;
        }
    }
    if (!fmtFound) {
        out_error = std::string(BufferFormatName(wantFmt)) + " not in HEVC input formats";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// QueryYuv444Support
// ---------------------------------------------------------------------------

bool NvencEncoder::QueryYuv444Support(std::string& out_error) {
    GUID codecGuid;
    const char* label;
    if (m_codec == VideoCodec::H264) {
        codecGuid = NV_ENC_CODEC_H264_GUID;
        label = "H264";
    } else if (m_codec == VideoCodec::Hevc) {
        codecGuid = NV_ENC_CODEC_HEVC_GUID;
        label = "HEVC";
    } else {
        // AV1 NVENC is 4:2:0 only — there is no 4:4:4 path to probe.
        out_error = "AV1 NVENC does not support 4:4:4 encoding";
        return false;
    }

    // Capability bit: NV_ENC_CAPS_SUPPORT_YUV444_ENCODE.
    int yuv444Cap = 0;
    std::string capsError;
    if (!QueryEncodeCap(m_funcs, m_encoder, codecGuid, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE, yuv444Cap, capsError)) {
        out_error = std::string(label) + " YUV444 caps query failed: " + capsError;
        return false;
    }
    if (yuv444Cap == 0) {
        out_error = std::string(label) + " reports NV_ENC_CAPS_SUPPORT_YUV444_ENCODE = 0";
        return false;
    }

    // Input-format enumeration: AYUV must be accepted for this codec.
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetInputFormatCount(m_encoder, codecGuid, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        out_error = std::string("nvEncGetInputFormatCount(") + label + "): " + NvencStatusName(st);
        return false;
    }
    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetInputFormats(m_encoder, codecGuid, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetInputFormats(") + label + "): " + NvencStatusName(st);
        return false;
    }
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_AYUV) {
            return true;
        }
    }
    out_error = std::string("NV_ENC_BUFFER_FORMAT_AYUV not in ") + label + " input formats";
    return false;
}

// ---------------------------------------------------------------------------
// ComputeNvencRcParams — pure mapping from canonical rate-control to NVENC
// ---------------------------------------------------------------------------
//
// NVENC SDK field names used here (NV_ENC_RC_PARAMS):
//   rcParams.rateControlMode   — NV_ENC_PARAMS_RC_CONSTQP / _VBR / _CBR
//   rcParams.constQP.qpIntra   — CQP I-frame quantizer (ConstantQuality)
//   rcParams.constQP.qpInterP  — CQP P-frame quantizer (ConstantQuality)
//   rcParams.constQP.qpInterB  — CQP B-frame quantizer (ConstantQuality; B=0 here)
//   rcParams.averageBitRate    — target average bitrate in bps (VBR/CBR)
//   rcParams.maxBitRate        — peak bitrate in bps (VBR: 1.5× avg; CBR: = avg)

RcParams ComputeNvencRcParams(RateControlMode mode, uint32_t cq, uint32_t bitrate_kbps) {
    RcParams p{};
    switch (mode) {
    case RateControlMode::ConstantQuality: {
        p.rateControlMode = static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP);
        // Out-of-range values are clamped rather than rejected: the encoder must
        // never be handed a QP outside [1, 51], whatever the caller passed.
        const uint32_t qp = cq < kCqMin ? kCqMin : (cq > kCqMax ? kCqMax : cq);
        // Inter frames carry +2 QP relative to intra — the ratio the three named
        // presets always used (19/21, 24/26, 30/32), now applied to every CQ.
        const uint32_t qp_inter = (qp + 2u) > kCqMax ? kCqMax : qp + 2u;
        p.qpIntra = qp;
        p.qpInterP = qp_inter;
        p.qpInterB = qp_inter;
        p.averageBitRate = 0;
        p.maxBitRate = 0;
        break;
    }
    case RateControlMode::VariableBitrate: {
        // VBR: encoder targets averageBitRate; allows peaks up to maxBitRate (1.5×).
        // rcParams.averageBitRate / rcParams.maxBitRate are in bps (not kbps).
        p.rateControlMode = static_cast<uint32_t>(NV_ENC_PARAMS_RC_VBR);
        p.averageBitRate = bitrate_kbps * 1000u;
        p.maxBitRate = bitrate_kbps * 1000u * 3u / 2u;
        p.qpIntra = 0;
        p.qpInterP = 0;
        p.qpInterB = 0;
        break;
    }
    case RateControlMode::ConstantBitrate: {
        // CBR: strict bitrate — averageBitRate == maxBitRate.
        p.rateControlMode = static_cast<uint32_t>(NV_ENC_PARAMS_RC_CBR);
        p.averageBitRate = bitrate_kbps * 1000u;
        p.maxBitRate = bitrate_kbps * 1000u;
        p.qpIntra = 0;
        p.qpInterP = 0;
        p.qpInterB = 0;
        break;
    }
    case RateControlMode::Lossless:
        // Lossless is not yet implemented. Capability marks it NotImplemented so
        // the UI hides it. Defensively fall back to ConstantQuality/Balanced.
        p = ComputeNvencRcParams(RateControlMode::ConstantQuality, CanonicalCq(QualityPreset::Balanced), bitrate_kbps);
        break;
    }
    return p;
}

// ---------------------------------------------------------------------------
// ComputeGopLength / ApplyGopToNvenc / ApplySpatialAqToNvenc / NextGopKeyframePhase
// Pure GOP + AQ helpers (see nvenc_encoder.h). No GPU/NVENC session required.
// ---------------------------------------------------------------------------
uint32_t ComputeGopLength(float keyframe_interval_secs, uint32_t frame_rate_num, uint32_t frame_rate_den) noexcept {
    if (frame_rate_num == 0u || frame_rate_den == 0u) {
        return 120u; // historical 2 s @ 60 fps fallback for a degenerate frame rate
    }
    const float secs = (keyframe_interval_secs > 0.0f) ? keyframe_interval_secs : 2.0f;
    const uint32_t gop =
        static_cast<uint32_t>(secs * static_cast<float>(frame_rate_num) / static_cast<float>(frame_rate_den) + 0.5f);
    return (gop > 0u) ? gop : 1u; // never 0 (which NVENC reads as an all-1-GOP infinite stream)
}

void ApplyGopToNvenc(NV_ENC_CONFIG& cfg, VideoCodec codec, uint32_t gop_length) noexcept {
    cfg.gopLength = gop_length;
    switch (codec) {
    case VideoCodec::H264:
        cfg.encodeCodecConfig.h264Config.idrPeriod = gop_length;
        break;
    case VideoCodec::Hevc:
        cfg.encodeCodecConfig.hevcConfig.idrPeriod = gop_length;
        break;
    default:
        cfg.encodeCodecConfig.av1Config.idrPeriod = gop_length;
        break;
    }
}

void ApplySpatialAqToNvenc(NV_ENC_CONFIG& cfg) noexcept {
    cfg.rcParams.enableAQ = 1;         // spatial AQ — no capability gate; safe without lookahead
    cfg.rcParams.enableTemporalAQ = 0; // deliberately off (undocumented without lookahead)
    cfg.rcParams.aqStrength = 0;       // 0 = driver auto-selects AQ strength
}

GopKeyframePhase NextGopKeyframePhase(uint32_t frame_in_gop, uint32_t gop_length, bool forced_idr) noexcept {
    GopKeyframePhase out;
    out.is_keyframe = forced_idr || (frame_in_gop == 0u);
    uint32_t f = out.is_keyframe ? 0u : frame_in_gop;
    ++f;
    if (gop_length > 0u && f >= gop_length) {
        f = 0u;
    }
    out.frame_in_gop = f;
    return out;
}

uint32_t ResyncGopPhaseFromActual(bool predicted_keyframe, bool actual_is_idr, uint32_t frame_in_gop) noexcept {
    return (actual_is_idr && !predicted_keyframe) ? 1u : frame_in_gop;
}

std::string FormatOutputTsMismatchError(uint64_t expected_output_ts, uint64_t actual_output_ts) {
    std::ostringstream oss;
    oss << "NVENC outputTimeStamp echo mismatch: expected=" << expected_output_ts << " actual=" << actual_output_ts
        << " (PTS assignment can no longer be trusted for this packet)";
    return oss.str();
}

std::string FormatKeyframePredictionMismatchWarning(bool predicted_keyframe, bool actual_keyframe) {
    std::ostringstream oss;
    oss << "NVENC keyframe prediction mismatch: predicted=" << (predicted_keyframe ? "keyframe" : "non-keyframe")
        << " actual=" << (actual_keyframe ? "keyframe" : "non-keyframe")
        << (actual_keyframe ? " (GOP phase resynced from actual IDR)" : " (GOP phase left as submitted)");
    return oss.str();
}

// ---------------------------------------------------------------------------
// FetchPresetConfig
// ---------------------------------------------------------------------------

bool NvencEncoder::FetchPresetConfig(std::string& out_error) {
    m_presetConfig = {};
    m_presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    m_presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    GUID codecGuid = NV_ENC_CODEC_AV1_GUID;
    if (m_codec == VideoCodec::H264) {
        codecGuid = NV_ENC_CODEC_H264_GUID;
    } else if (m_codec == VideoCodec::Hevc) {
        codecGuid = NV_ENC_CODEC_HEVC_GUID;
    }

    // NVENC speed/quality preset (P1..P7) — user-selectable expert setting,
    // default P4. Resolved via the pure NvencPresetToGuid mapping and applied
    // uniformly across all three codecs (no per-codec gating). NOTE: P5-P7 on
    // AV1/HEVC previously triggered NV_ENC_ERR_NEED_MORE_INPUT on every frame
    // even with lookahead disabled (internal pipeline depth); EncodeFrame
    // already buffers/drains this case (m_pending FIFO), so it is
    // not fatal, but it increases encode latency and 8-slot input-ring pressure.
    m_presetGuid = NvencPresetToGuid(m_preset);

    NVENCSTATUS st =
        m_funcs.nvEncGetEncodePresetConfigEx(m_encoder, codecGuid, m_presetGuid, m_tuningInfo, &m_presetConfig);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetEncodePresetConfigEx: ") + NvencStatusName(st);
        return false;
    }

    std::memcpy(&m_encodeConfig, &m_presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    m_encodeConfig.version = NV_ENC_CONFIG_VER;

    // 10-bit (P010 input → Main10 / AV1 10-bit) selects the appropriate codec profile
    // and per-codec input/output bit-depth fields. NV_ENC_BIT_DEPTH_8 is the default
    // (left implicit by the preset) for the 8-bit path, so we only set fields for 10-bit.
    // H.264 never reaches here for 10-bit — it is rejected upstream in Validate().
    const bool tenBit = (m_bitDepth == BitDepth::Bit10);
    const NV_ENC_BIT_DEPTH nvBitDepth = tenBit ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;

    // Chroma: 1 = 4:2:0 (NV12/P010), 3 = 4:4:4 (AYUV). 4:4:4 is an 8-bit
    // H.264/HEVC expert option; AV1 and 4:4:4 + 10-bit are rejected upstream, so
    // this stays at 1 for AV1 and for every 4:2:0 session — keeping the 4:2:0
    // bitstream byte-identical to before.
    const uint32_t chromaIdc = NvencChromaFormatIDC(m_chroma);
    const bool is444 = (m_chroma == ChromaSubsampling::Cs444);

    if (m_codec == VideoCodec::H264) {
        m_encodeConfig.encodeCodecConfig.h264Config.chromaFormatIDC = chromaIdc;
        if (is444)
            m_encodeConfig.profileGUID = Nvenc444ProfileGuid(m_codec); // High 4:4:4 Predictive
    } else if (m_codec == VideoCodec::Hevc) {
        m_encodeConfig.encodeCodecConfig.hevcConfig.chromaFormatIDC = chromaIdc; // YUV420/P010 or YUV444/AYUV
        m_encodeConfig.encodeCodecConfig.hevcConfig.inputBitDepth = nvBitDepth;
        m_encodeConfig.encodeCodecConfig.hevcConfig.outputBitDepth = nvBitDepth;
        if (tenBit)
            m_encodeConfig.profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
        else if (is444)
            m_encodeConfig.profileGUID = Nvenc444ProfileGuid(m_codec); // FREXT (Range Extensions)
    } else {
        m_encodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1; // YUV420/NV12 or P010
        m_encodeConfig.encodeCodecConfig.av1Config.inputBitDepth = nvBitDepth;
        m_encodeConfig.encodeCodecConfig.av1Config.outputBitDepth = nvBitDepth;
        // AV1 uses a single Main profile GUID for both 8- and 10-bit; the bit depth is
        // signaled by the input/output bit-depth fields above, not a distinct profile.
        if (tenBit)
            m_encodeConfig.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
    }

    // Color signaling (fix for color-range-signaling bug): populate the
    // codec-specific bitstream color fields from the same ColorMetadata that
    // drives the VideoProcessor conversion and the Matroska Colour element, so
    // the bitstream is never color-ambiguous. See ApplyColorMetadataToNvenc.
    ApplyColorMetadataToNvenc(m_encodeConfig, m_codec, m_color);

    // Apply canonical rate-control via the pure, testable ComputeNvencRcParams helper.
    // NVENC SDK field names: rcParams.rateControlMode / constQP / averageBitRate / maxBitRate.
    const RcParams rc = ComputeNvencRcParams(m_rateControlMode, m_cq, m_bitrate_kbps);
    m_encodeConfig.rcParams.rateControlMode = static_cast<NV_ENC_PARAMS_RC_MODE>(rc.rateControlMode);
    m_encodeConfig.rcParams.constQP.qpIntra = rc.qpIntra;
    m_encodeConfig.rcParams.constQP.qpInterP = rc.qpInterP;
    m_encodeConfig.rcParams.constQP.qpInterB = rc.qpInterB;
    m_encodeConfig.rcParams.averageBitRate = rc.averageBitRate;
    m_encodeConfig.rcParams.maxBitRate = rc.maxBitRate;

    // Zero lookahead and P-only: prevents 8-slot NVENC input ring from exhausting.
    // AV1 P4 constraint — do not change frameIntervalP.
    m_encodeConfig.rcParams.enableLookahead = 0;
    m_encodeConfig.rcParams.lookaheadDepth = 0;
    m_encodeConfig.frameIntervalP = 1;

    // Explicitly pin spatial adaptive quantization on, so the AQ state no longer
    // depends on the driver's per-preset default. Temporal AQ stays off (no
    // lookahead) — see ApplySpatialAqToNvenc.
    ApplySpatialAqToNvenc(m_encodeConfig);

    return true;
}

// ---------------------------------------------------------------------------
// BuildHdrBitstreamPayloads
// ---------------------------------------------------------------------------
//
// Precompute the per-keyframe in-band HDR10 metadata once, so EncodeFrame only
// has to point NVENC at owned, stable buffers. HEVC uses SEI messages
// (seiPayloadArray, payloadType 137/144); AV1 uses metadata OBUs
// (obuPayloadArray, payloadType = AV1 metadata_type 2/1). For both codecs NVENC
// consumes the same NV_ENC_SEI_PAYLOAD descriptor: {payloadSize, payloadType,
// payload}. We supply the payload *content* only; NVENC frames it (SEI NAL +
// emulation prevention for HEVC; OBU header + leb128 size + metadata_type +
// byte alignment for AV1). See recorder_core/hdr_bitstream_metadata.h.
void NvencEncoder::BuildHdrBitstreamPayloads() {
    m_hdrPayloadCount = 0;
    m_hdrMdcvPayload.clear();
    m_hdrCllPayload.clear();

    // H.264 never carries HDR10-native (blocked upstream); only HEVC/AV1 do.
    if (m_codec != VideoCodec::Hevc && m_codec != VideoCodec::Av1) {
        return;
    }
    if (!hdr_meta::ShouldEmitHdrBitstreamMetadata(m_color)) {
        return;
    }
    const bool av1 = (m_codec == VideoCodec::Av1);

    if (hdr_meta::HasMasteringDisplayData(m_color)) {
        m_hdrMdcvPayload = av1 ? hdr_meta::BuildAv1MasteringDisplayObuPayload(m_color)
                               : hdr_meta::BuildHevcMasteringDisplaySeiPayload(m_color);
        NV_ENC_SEI_PAYLOAD& e = m_hdrPayloadEntries[m_hdrPayloadCount++];
        e.payloadSize = static_cast<uint32_t>(m_hdrMdcvPayload.size());
        e.payloadType = av1 ? hdr_meta::kAv1MetadataTypeHdrMdcv : hdr_meta::kHevcSeiPayloadTypeMasteringDisplay;
        e.payload = m_hdrMdcvPayload.data();
    }
    if (hdr_meta::HasContentLightLevelData(m_color)) {
        m_hdrCllPayload = av1 ? hdr_meta::BuildAv1ContentLightLevelObuPayload(m_color)
                              : hdr_meta::BuildHevcContentLightLevelSeiPayload(m_color);
        NV_ENC_SEI_PAYLOAD& e = m_hdrPayloadEntries[m_hdrPayloadCount++];
        e.payloadSize = static_cast<uint32_t>(m_hdrCllPayload.size());
        e.payloadType = av1 ? hdr_meta::kAv1MetadataTypeHdrCll : hdr_meta::kHevcSeiPayloadTypeContentLightLevel;
        e.payload = m_hdrCllPayload.data();
    }
}

// ---------------------------------------------------------------------------
// InitEncoder
// ---------------------------------------------------------------------------

bool NvencEncoder::InitEncoder(uint32_t width, uint32_t height, uint32_t frame_rate_num, uint32_t frame_rate_den,
                               std::string& out_error) {
    // Keyframe interval: gopLength = round(interval_secs * fps), applied together
    // with the codec-specific idrPeriod. m_keyframeIntervalSecs is set from the
    // user's Settings → Advanced selection (via SetKeyframeIntervalSecs) and
    // defaults to 2.0 (pre-0.9.0 hardcoded behaviour when unset).
    const uint32_t kGopFrames = ComputeGopLength(m_keyframeIntervalSecs, frame_rate_num, frame_rate_den);
    ApplyGopToNvenc(m_encodeConfig, m_codec, kGopFrames);
    // Remember the IDR cadence and (re)build the HDR metadata payloads for this
    // session. m_frameInGop starts at 0 so the first submitted frame — always an
    // IDR — carries the metadata. The submission-side keyframe prediction in
    // EncodeFrame relies on this cadence AND on frameIntervalP = 1 / no lookahead
    // / no adaptive I: enabling any of those desynchronizes the predicted GOP
    // phase from NVENC's real IDR placement (metadata would land on non-IDR
    // frames — legal but off-cadence). Revisit the prediction if that changes.
    m_gopLength = kGopFrames;
    m_frameInGop = 0;
    m_loggedKeyframePredictionMismatch = false;
    BuildHdrBitstreamPayloads();

    GUID codecGuid = NV_ENC_CODEC_AV1_GUID;
    if (m_codec == VideoCodec::H264)
        codecGuid = NV_ENC_CODEC_H264_GUID;
    else if (m_codec == VideoCodec::Hevc)
        codecGuid = NV_ENC_CODEC_HEVC_GUID;

    // codec_index: 0=AV1, 1=H264, 2=HEVC — used in diag string and error messages
    const int codec_index = (m_codec == VideoCodec::H264) ? 1 : (m_codec == VideoCodec::Hevc) ? 2 : 0;

    int capWidthMin = -1;
    int capWidthMax = -1;
    int capHeightMin = -1;
    int capHeightMax = -1;
    std::string capsError;

    const bool haveWidthMin =
        QueryEncodeCap(m_funcs, m_encoder, codecGuid, NV_ENC_CAPS_WIDTH_MIN, capWidthMin, capsError);
    const bool haveWidthMax =
        QueryEncodeCap(m_funcs, m_encoder, codecGuid, NV_ENC_CAPS_WIDTH_MAX, capWidthMax, capsError);
    const bool haveHeightMin =
        QueryEncodeCap(m_funcs, m_encoder, codecGuid, NV_ENC_CAPS_HEIGHT_MIN, capHeightMin, capsError);
    const bool haveHeightMax =
        QueryEncodeCap(m_funcs, m_encoder, codecGuid, NV_ENC_CAPS_HEIGHT_MAX, capHeightMax, capsError);
    const bool haveCaps = haveWidthMin && haveWidthMax && haveHeightMin && haveHeightMax;

    // Capability gate: async mode only if the hardware/driver advertises it.
    // Absent/failed query -> m_asyncMode stays false and the existing sync
    // path runs unchanged — this is not itself an error.
    int capAsync = 0;
    std::string asyncCapsError;
    m_asyncMode =
        QueryEncodeCap(m_funcs, m_encoder, codecGuid, NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT, capAsync, asyncCapsError) &&
        capAsync != 0;

    NV_ENC_INITIALIZE_PARAMS p{};
    p.version = NV_ENC_INITIALIZE_PARAMS_VER;
    p.encodeGUID = codecGuid;
    p.presetGUID = m_presetGuid;
    p.tuningInfo = m_tuningInfo;
    p.encodeWidth = width;
    p.encodeHeight = height;
    p.darWidth = width;
    p.darHeight = height;
    p.maxEncodeWidth = width;
    p.maxEncodeHeight = height;
    p.frameRateNum = frame_rate_num;
    p.frameRateDen = frame_rate_den;
    p.enablePTD = 1;
    p.enableEncodeAsync = m_asyncMode ? 1 : 0;
    p.encodeConfig = &m_encodeConfig;

    const bool evenWidth = (width % 2u) == 0u;
    const bool evenHeight = (height % 2u) == 0u;
    const bool widthInRange =
        !haveCaps || (width >= static_cast<uint32_t>(capWidthMin) && width <= static_cast<uint32_t>(capWidthMax));
    const bool heightInRange =
        !haveCaps || (height >= static_cast<uint32_t>(capHeightMin) && height <= static_cast<uint32_t>(capHeightMax));

    const std::string initDiag = BuildInitDiagString(p, m_encodeConfig, codec_index, haveCaps, capWidthMin, capWidthMax,
                                                     capHeightMin, capHeightMax, m_bitDepth, m_chroma);

    // Honest 4:4:4 gate: refuse before nvEncInitializeEncoder if the GPU does not
    // advertise YUV444 encoding for this codec, so we fail with a clear message
    // rather than mis-encoding. Only checked for the 4:4:4 path; the 4:2:0 path is
    // untouched.
    if (m_chroma == ChromaSubsampling::Cs444) {
        std::string yuv444Err;
        if (!QueryYuv444Support(yuv444Err)) {
            out_error = std::string("NVENC ") + CodecLabel(codec_index) + " 4:4:4 unsupported: " + yuv444Err +
                        "; init={" + initDiag + "}";
            return false;
        }
    }

    if (!evenWidth || !evenHeight || !widthInRange || !heightInRange) {
        std::ostringstream oss;
        oss << "NVENC " << CodecLabel(codec_index)
            << " init dimension sanity failed: evenWidth=" << (evenWidth ? "true" : "false")
            << ", evenHeight=" << (evenHeight ? "true" : "false")
            << ", widthInRange=" << (widthInRange ? "true" : "false")
            << ", heightInRange=" << (heightInRange ? "true" : "false") << "; init={" << initDiag << "}";
        if (!haveCaps && !capsError.empty()) {
            oss << "; capsQueryError=" << capsError;
        }
        out_error = oss.str();
        return false;
    }

    NVENCSTATUS st = m_funcs.nvEncInitializeEncoder(m_encoder, &p);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncInitializeEncoder: ") + NvencStatusName(st) + "; init={" + initDiag + "}";
        if (!haveCaps && !capsError.empty()) {
            out_error += "; capsQueryError=" + capsError;
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CreateBitstreamBuffer
// ---------------------------------------------------------------------------

bool NvencEncoder::CreateBitstreamBuffer(std::string& out_error) {
    if (!m_asyncMode) {
        NV_ENC_CREATE_BITSTREAM_BUFFER bsp{};
        bsp.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        NVENCSTATUS st = m_funcs.nvEncCreateBitstreamBuffer(m_encoder, &bsp);
        if (st != NV_ENC_SUCCESS) {
            out_error = std::string("nvEncCreateBitstreamBuffer: ") + NvencStatusName(st);
            return false;
        }
        m_bitstreamBuffer = bsp.bitstreamBuffer;
        return true;
    }

    // Async mode: kMaxOutputResources bitstream buffers + one auto-reset
    // completion event each, registered with the driver, plus one reserved
    // EOS event. All kMaxOutputResources are allocated regardless of
    // m_activeDepth — see the m_outputResources member comment.
    for (int32_t i = 0; i < kMaxOutputResources; ++i) {
        NV_ENC_CREATE_BITSTREAM_BUFFER bsp{};
        bsp.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        NVENCSTATUS st = m_funcs.nvEncCreateBitstreamBuffer(m_encoder, &bsp);
        if (st != NV_ENC_SUCCESS) {
            out_error = std::string("nvEncCreateBitstreamBuffer[") + std::to_string(i) + "]: " + NvencStatusName(st);
            DestroyOutputRing();
            return false;
        }
        m_outputResources[i].bitstream = bsp.bitstreamBuffer;

        HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev == nullptr) {
            out_error = "CreateEventW failed for output-ring slot " + std::to_string(i);
            DestroyOutputRing();
            return false;
        }
        NV_ENC_EVENT_PARAMS ep{};
        ep.version = NV_ENC_EVENT_PARAMS_VER;
        ep.completionEvent = ev;
        st = m_funcs.nvEncRegisterAsyncEvent(m_encoder, &ep);
        if (st != NV_ENC_SUCCESS) {
            out_error = std::string("nvEncRegisterAsyncEvent[") + std::to_string(i) + "]: " + NvencStatusName(st);
            ::CloseHandle(ev);
            DestroyOutputRing();
            return false;
        }
        m_outputResources[i].event = ev;
    }

    m_eosEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_eosEvent == nullptr) {
        out_error = "CreateEventW failed for the reserved EOS event";
        DestroyOutputRing();
        return false;
    }
    NV_ENC_EVENT_PARAMS eosEp{};
    eosEp.version = NV_ENC_EVENT_PARAMS_VER;
    eosEp.completionEvent = m_eosEvent;
    const NVENCSTATUS eosSt = m_funcs.nvEncRegisterAsyncEvent(m_encoder, &eosEp);
    if (eosSt != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncRegisterAsyncEvent[EOS]: ") + NvencStatusName(eosSt);
        DestroyOutputRing();
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// DestroyOutputRing (internal)
// ---------------------------------------------------------------------------

void NvencEncoder::DestroyOutputRing() noexcept {
    // Teardown order: unregister + close every event first, then destroy
    // every bitstream buffer. Safe on partial state (rollback from a failed
    // CreateBitstreamBuffer) and safe to call more than once.
    for (auto& res : m_outputResources) {
        if (m_encoder && m_funcs.nvEncUnregisterAsyncEvent && res.event) {
            NV_ENC_EVENT_PARAMS ep{};
            ep.version = NV_ENC_EVENT_PARAMS_VER;
            ep.completionEvent = res.event;
            m_funcs.nvEncUnregisterAsyncEvent(m_encoder, &ep);
        }
        if (res.event) {
            ::CloseHandle(res.event);
            res.event = nullptr;
        }
    }
    if (m_eosEvent) {
        if (m_encoder && m_funcs.nvEncUnregisterAsyncEvent) {
            NV_ENC_EVENT_PARAMS ep{};
            ep.version = NV_ENC_EVENT_PARAMS_VER;
            ep.completionEvent = m_eosEvent;
            m_funcs.nvEncUnregisterAsyncEvent(m_encoder, &ep);
        }
        ::CloseHandle(m_eosEvent);
        m_eosEvent = nullptr;
    }
    for (auto& res : m_outputResources) {
        if (m_encoder && m_funcs.nvEncDestroyBitstreamBuffer && res.bitstream) {
            m_funcs.nvEncDestroyBitstreamBuffer(m_encoder, res.bitstream);
            res.bitstream = nullptr;
        }
        res.in_flight = false;
    }
}

// ---------------------------------------------------------------------------
// RegisterSlotTexture
// ---------------------------------------------------------------------------

bool NvencEncoder::RegisterSlotTexture(int32_t slot_idx, ID3D11Texture2D* texture, std::string& out_error) {
    if (slot_idx < 0 || slot_idx >= 8) {
        out_error = "RegisterSlotTexture: slot_idx out of range [0,7]";
        return false;
    }
    if (m_slots[slot_idx].registeredResource != nullptr) {
        out_error = "RegisterSlotTexture: slot already registered";
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = desc.Width;
    reg.height = desc.Height;
    reg.pitch = 0;
    reg.subResourceIndex = 0;
    reg.resourceToRegister = texture;
    // 8-bit registers the NV12 D3D11 texture; 10-bit registers the P010 texture as
    // NV_ENC_BUFFER_FORMAT_YUV420_10BIT (both are semi-planar 4:2:0, P010 being 16 bpc).
    reg.bufferFormat = NvencInputFormat(m_bitDepth, m_chroma);
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS st = m_funcs.nvEncRegisterResource(m_encoder, &reg);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncRegisterResource: ") + NvencStatusName(st);
        return false;
    }
    m_slots[slot_idx].registeredResource = reg.registeredResource;
    if (!m_slots[slot_idx].registeredResource) {
        out_error = "registeredResource is null after successful nvEncRegisterResource";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AcquireFreeSlot
// ---------------------------------------------------------------------------

int32_t NvencEncoder::AcquireFreeSlot() {
    for (int i = 0; i < 8; ++i) {
        int32_t idx = (m_slotCursor + i) % 8;
        InputSlot& slot = m_slots[idx];
        if (!slot.in_flight && !slot.mapped) {
            slot.in_flight = true;
            m_slotCursor = (idx + 1) % 8;
            return idx;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// ReleaseSlot
// ---------------------------------------------------------------------------

void NvencEncoder::ReleaseSlot(int32_t slot_idx) noexcept {
    if (slot_idx < 0 || slot_idx >= 8)
        return;
    InputSlot& slot = m_slots[slot_idx];
    if (slot.mapped && slot.mappedResource != nullptr) {
        m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
        slot.mappedResource = nullptr;
    }
    slot.mapped = false;
    slot.in_flight = false;
}

// ---------------------------------------------------------------------------
// LockAndConsumeBitstream (internal)
// ---------------------------------------------------------------------------

bool NvencEncoder::LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error, bool non_blocking,
                                           NVENCSTATUS* out_lock_status) {
    // Async mode: the FIFO head names its own output-ring bitstream (chosen at
    // submission time, see EncodeFrame); sync mode always uses the single
    // shared buffer. Peeked before locking because which buffer to lock
    // depends on it.
    NV_ENC_OUTPUT_PTR bitstreamToLock = m_bitstreamBuffer;
    if (m_asyncMode && !m_pending.empty()) {
        const int32_t headOutIdx = m_pending.front().out_idx;
        if (headOutIdx >= 0 && headOutIdx < kMaxOutputResources)
            bitstreamToLock = m_outputResources[headOutIdx].bitstream;
    }

    NV_ENC_LOCK_BITSTREAM lockBS{};
    lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBS.outputBitstream = bitstreamToLock;
    lockBS.doNotWait = non_blocking ? 1 : 0;

    NVENCSTATUS st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
    if (out_lock_status != nullptr)
        *out_lock_status = st;
    if (st != NV_ENC_SUCCESS) {
        // On LOCK_BUSY nothing has been popped/consumed yet (the pending PTS/slot
        // are still queued), so a non-blocking caller can safely retry.
        out_error = std::string("nvEncLockBitstream: ") + NvencStatusName(st);
        return false;
    }

    uint64_t ts_ns = 0;
    double latency_ms = -1.0;
    bool outputTsMismatch = false;
    bool keyframePredictionMismatch = false;
    const bool actualIsIdr = (lockBS.pictureType == NV_ENC_PIC_TYPE_IDR);
    if (!m_pending.empty()) {
        const PendingFrame pf = m_pending.front();
        m_pending.pop();
        ts_ns = pf.pts_ns;
        // True submit -> bitstream-available latency for this frame. In the P5-P7
        // buffered case the consumed output belongs to an earlier submission, so
        // this is the only place the correct latency can be measured (the video
        // thread's call-site bracket would attribute it to the wrong frame).
        latency_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pf.submit_time).count();

        // outputTimeStamp is expected to echo the inputTimeStamp of the frame
        // that produced this bitstream (verified live on real hardware across
        // all three codecs and both P4/P7 presets, 0 mismatches over 360
        // frames; FFmpeg's NVENC wrapper also trusts this echo unconditionally,
        // with no cross-check, across its entire supported hardware range). A
        // mismatch means PTS assignment can no longer be trusted for this
        // packet — treated as data corruption, not a recoverable warning. The
        // slot/pending cleanup above has already happened, so it is safe to
        // abort here.
        outputTsMismatch = (lockBS.outputTimeStamp != pf.input_ts);
        if (outputTsMismatch) {
            const std::string mismatchMsg = FormatOutputTsMismatchError(pf.input_ts, lockBS.outputTimeStamp);
            logging::log(logging::LogLevel::Error, "nvenc.order_validation", mismatchMsg);
            m_funcs.nvEncUnlockBitstream(m_encoder, bitstreamToLock);
            out_error = mismatchMsg;
            return false;
        }

        // Keyframe-prediction validation. When the actual pictureType confirms
        // the prediction, the submission-side GOP phase stays untouched — it is
        // several frames ahead of this packet under async buffering, and
        // rewinding it from here stretched every GOP by the in-flight depth.
        // Only an unpredicted real IDR resyncs the phase; a mismatch is
        // logged/counted, never fatal at this stage — the actual pictureType
        // (isKey below) remains authoritative for muxing regardless.
        keyframePredictionMismatch = (pf.predicted_keyframe != actualIsIdr);
        if (keyframePredictionMismatch && !m_loggedKeyframePredictionMismatch) {
            m_loggedKeyframePredictionMismatch = true;
            logging::log(logging::LogLevel::Warn, "nvenc.order_validation",
                         FormatKeyframePredictionMismatchWarning(pf.predicted_keyframe, actualIsIdr));
        }
        m_frameInGop = ResyncGopPhaseFromActual(pf.predicted_keyframe, actualIsIdr, m_frameInGop);

        // Release the associated input slot.
        if (pf.slot_idx >= 0 && pf.slot_idx < 8) {
            InputSlot& slot = m_slots[pf.slot_idx];
            if (slot.mapped && slot.mappedResource != nullptr) {
                m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
                slot.mappedResource = nullptr;
            }
            slot.mapped = false;
            slot.in_flight = false;
        }

        // Async mode: this output-ring slot is free again for a future submission.
        if (m_asyncMode && pf.out_idx >= 0 && pf.out_idx < kMaxOutputResources) {
            m_outputResources[pf.out_idx].in_flight = false;
        }
    }

    bool isKey = actualIsIdr || (lockBS.pictureType == NV_ENC_PIC_TYPE_I);

    out_packet.pts_ns = ts_ns;
    out_packet.keyframe = isKey;
    out_packet.encode_latency_ms = latency_ms;
    out_packet.output_ts_mismatch = outputTsMismatch;
    out_packet.keyframe_prediction_mismatch = keyframePredictionMismatch;
    out_packet.bytes.assign(static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                            static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes);

    m_funcs.nvEncUnlockBitstream(m_encoder, bitstreamToLock);
    return true;
}

// ---------------------------------------------------------------------------
// WaitAndConsumeOneAsync (internal)
// ---------------------------------------------------------------------------

EventDrainStep NvencEncoder::WaitAndConsumeOneAsync(HANDLE event, double budget_ms, EncodedVideoPacket& out_packet,
                                                    std::string& out_error) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const DWORD waitResult = ::WaitForSingleObject(event, 20);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const EventDrainStep step = NextEventDrainStep(waitResult, elapsedMs, budget_ms);
        if (step == EventDrainStep::Consume) {
            std::string lockErr;
            if (!LockAndConsumeBitstream(out_packet, lockErr, /*non_blocking=*/true)) {
                // The event fired but the lock still failed (e.g. device lost
                // between signal and lock) — surface as an error, not a silent
                // no-op, since the caller expects one of {Consume, Abort*}.
                out_error = lockErr;
                return EventDrainStep::AbortError;
            }
            return EventDrainStep::Consume;
        }
        if (step == EventDrainStep::Retry)
            continue;
        out_error = (step == EventDrainStep::AbortTimeout)
                        ? "WaitAndConsumeOneAsync: timed out waiting for the completion event"
                        : "WaitAndConsumeOneAsync: WaitForSingleObject failed";
        return step;
    }
}

// ---------------------------------------------------------------------------
// EncodeFrame
// ---------------------------------------------------------------------------

bool NvencEncoder::EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                               std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) {
    if (slot_idx < 0 || slot_idx >= 8) {
        out_error = "EncodeFrame: slot_idx out of range [0,7]";
        return false;
    }
    InputSlot& slot = m_slots[slot_idx];
    if (slot.mapped) {
        out_error = "EncodeFrame: slot already mapped";
        return false;
    }

    // Async mode: ensure a free output-ring slot before touching the input
    // side. If the ring (m_activeDepth slots) is full, bounded-wait on the
    // oldest pending frame's completion event and reap it — freeing exactly
    // one slot — before proceeding; that reaped packet is a legitimate output
    // of this submission call, appended alongside whatever this submission
    // itself produces. Bounded by m_pending shrinking by one per iteration, so
    // this always terminates (each WaitAndConsumeOneAsync call is itself
    // budget-bounded).
    int32_t out_idx = -1;
    if (m_asyncMode) {
        for (;;) {
            bool inFlight[kMaxOutputResources] = {};
            for (int32_t i = 0; i < m_activeDepth; ++i)
                inFlight[i] = m_outputResources[i].in_flight;
            const FreeOutputSlotResult free = FindFreeOutputSlot(inFlight, m_activeDepth, m_outputCursor);
            if (free.slot_idx >= 0) {
                out_idx = free.slot_idx;
                m_outputCursor = free.next_cursor;
                break;
            }
            if (m_pending.empty()) {
                out_error = "EncodeFrame (async): output ring full but no pending frame to wait on";
                return false;
            }
            constexpr double kSlotWaitBudgetMs = 2000.0;
            EncodedVideoPacket drained;
            std::string waitErr;
            const HANDLE headEvent = m_outputResources[m_pending.front().out_idx].event;
            const EventDrainStep step = WaitAndConsumeOneAsync(headEvent, kSlotWaitBudgetMs, drained, waitErr);
            if (step != EventDrainStep::Consume) {
                out_error = waitErr;
                return false;
            }
            out_packets.push_back(std::move(drained));
        }
    }

    // Map this slot's registered NV12 resource
    NV_ENC_MAP_INPUT_RESOURCE mapRes{};
    mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = slot.registeredResource;

    NVENCSTATUS st = m_funcs.nvEncMapInputResource(m_encoder, &mapRes);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncMapInputResource: ") + NvencStatusName(st);
        return false;
    }
    slot.mappedResource = mapRes.mappedResource;
    slot.mapped = true;

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.inputPitch = 0;
    pic.inputBuffer = mapRes.mappedResource;
    pic.bufferFmt = mapRes.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    // One-shot segment-boundary request (RequestKeyframe()), consumed now
    // regardless of the cadence outcome below — it always feeds into this
    // submission's phase decision via NextGopKeyframePhase's forced_idr param.
    const bool forcedIdr = m_forceIdrNext;
    m_forceIdrNext = false;

    // Keyframe cadence is now an ENFORCED fact, not a prediction about NVENC's
    // internal idrPeriod timer. NextGopKeyframePhase is the pure decision
    // function (tested with non-default GOP lengths, nvenc_encoder.h); it
    // honours the configured m_gopLength (a user-selected 1 s / 0.5 s keyframe
    // interval), and folds in the one-shot forced-IDR request. Whatever it
    // decides, we drive it here with NV_ENC_PIC_FLAG_FORCEIDR — idrPeriod stays
    // set as a belt-and-braces backstop, but is no longer the mechanism the
    // keyframe positions actually depend on.
    const GopKeyframePhase phase = NextGopKeyframePhase(m_frameInGop, m_gopLength, forcedIdr);
    const bool isKeyframe = phase.is_keyframe;
    m_frameInGop = phase.frame_in_gop;

    if (isKeyframe) {
        pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
    }

    // Attach the precomputed in-band HDR10 metadata on every keyframe, so each
    // segment/split file and mid-stream join point carries it. The payload
    // buffers are owned members that outlive this synchronous encode call.
    if (isKeyframe && m_hdrPayloadCount > 0) {
        if (m_codec == VideoCodec::Av1) {
            pic.codecPicParams.av1PicParams.obuPayloadArray = m_hdrPayloadEntries.data();
            pic.codecPicParams.av1PicParams.obuPayloadArrayCnt = m_hdrPayloadCount;
        } else {
            pic.codecPicParams.hevcPicParams.seiPayloadArray = m_hdrPayloadEntries.data();
            pic.codecPicParams.hevcPicParams.seiPayloadArrayCnt = m_hdrPayloadCount;
        }
    }
    pic.inputTimeStamp = m_frameIdx++;

    if (m_asyncMode) {
        pic.outputBitstream = m_outputResources[out_idx].bitstream;
        pic.completionEvent = m_outputResources[out_idx].event;
        m_outputResources[out_idx].in_flight = true;
    } else {
        pic.outputBitstream = m_bitstreamBuffer;
    }

    // Record this submission before the encode call (FIFO: one entry per submitted
    // frame). submit_time stamps the start of the encode so the consuming lock can
    // report the true per-frame latency, independent of preset buffering. input_ts
    // and predicted_keyframe are captured here (submission-side truth) so the
    // consuming lock can validate them against the driver's actual
    // outputTimeStamp / pictureType. out_idx is -1 in sync mode (unused).
    m_pending.push(
        PendingFrame{pts_ns, slot_idx, std::chrono::steady_clock::now(), pic.inputTimeStamp, isKeyframe, out_idx});

    st = m_funcs.nvEncEncodePicture(m_encoder, &pic);

    if (st == NV_ENC_SUCCESS) {
        if (m_asyncMode) {
            // Submit-only: this frame's own output arrives later via its
            // completion event (ReapCompleted, or a future submission's
            // output-ring-full wait) — nothing to consume synchronously.
            return true;
        }
        // Sync mode: output available immediately (unchanged). Lock pops the
        // earliest pending PTS+slot and releases that slot (unmap + mark free).
        EncodedVideoPacket pkt;
        std::string lockErr;
        if (!LockAndConsumeBitstream(pkt, lockErr)) {
            // Bitstream lock failed: clean up the current slot.
            m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
            slot.mappedResource = nullptr;
            slot.mapped = false;
            slot.in_flight = false;

            // Pop our own pending entry; LockAndConsumeBitstream may have
            // already popped it if it got partway through. Best-effort: drain
            // one entry if present.
            if (!m_pending.empty()) {
                m_pending.pop();
            }
            out_error = lockErr;
            return false;
        }
        out_packets.push_back(std::move(pkt));
        return true;
    } else if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        // Buffered — PTS and slot are queued; do not unmap. Not expected in
        // async mode (events signal completion instead of this status), but
        // tolerated exactly like today if the driver still returns it.
        ++m_needMoreInputCount;
        return true;
    } else {
        // Fatal encode error — clean up this slot
        m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
        slot.mappedResource = nullptr;
        slot.mapped = false;
        slot.in_flight = false;

        // This submission will never complete — free its output-ring slot
        // back rather than leaving it permanently marked in-flight.
        if (m_asyncMode && out_idx >= 0 && out_idx < kMaxOutputResources) {
            m_outputResources[out_idx].in_flight = false;
        }

        // Remove the entry we just pushed (best-effort, front of queue)
        if (!m_pending.empty())
            m_pending.pop();

        out_error = std::string("nvEncEncodePicture: ") + NvencStatusName(st);
        return false;
    }
}

// ---------------------------------------------------------------------------
// ReapCompleted
// ---------------------------------------------------------------------------

bool NvencEncoder::ReapCompleted(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error,
                                 uint32_t wait_head_ms) {
    if (!m_asyncMode) {
        // Sync mode: output is always consumed inline by EncodeFrame — nothing
        // to drain here. Matches the IVideoEncoder default no-op.
        return true;
    }

    // Wait up to wait_head_ms for the oldest pending frame only; every
    // further packet in the same call is drained without additional waiting
    // (a zero-timeout poll) — stop as soon as one is not yet ready.
    bool first = true;
    while (!m_pending.empty()) {
        const int32_t headOutIdx = m_pending.front().out_idx;
        if (headOutIdx < 0 || headOutIdx >= kMaxOutputResources) {
            out_error = "ReapCompleted: pending frame has an invalid output-ring index";
            return false;
        }
        const HANDLE event = m_outputResources[headOutIdx].event;
        const DWORD waitResult = ::WaitForSingleObject(event, first ? static_cast<DWORD>(wait_head_ms) : 0);
        first = false;
        if (waitResult != WAIT_OBJECT_0) {
            if (waitResult == WAIT_FAILED) {
                out_error = "ReapCompleted: WaitForSingleObject failed";
                return false;
            }
            // WAIT_TIMEOUT (or an unexpected-but-non-fatal result): nothing
            // more is ready right now — not an error, just stop draining.
            break;
        }
        EncodedVideoPacket pkt;
        std::string lockErr;
        if (!LockAndConsumeBitstream(pkt, lockErr, /*non_blocking=*/true)) {
            out_error = lockErr;
            return false;
        }
        out_packets.push_back(std::move(pkt));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

bool NvencEncoder::Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) {
    if (m_asyncMode)
        return FlushAsync(out_packets, out_error);

    // Send EOS
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eos);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncEncodePicture(EOS): ") + NvencStatusName(st);
        return false;
    }

    // Drain buffered frames with a bounded, non-blocking poll. A blocking lock
    // here (the historic doNotWait=0 path) hangs forever when the device is lost
    // or hung — the buffered outputs never complete — so the video thread wedged
    // and the whole session died to the fixed join budget. Polling with a per-
    // frame time budget guarantees the drain always terminates: a healthy device
    // delivers each frame in well under the budget (progress resets the clock),
    // while a dead one is abandoned after the budget and the caller still pushes
    // EOS and finalises. LockAndConsumeBitstream releases each slot on consume.
    constexpr double kFlushDrainBudgetMs = 2000.0;
    auto lastProgress = std::chrono::steady_clock::now();
    for (int i = 0; i < m_needMoreInputCount;) {
        if (m_pending.empty())
            break;

        EncodedVideoPacket pkt;
        std::string lockErr;
        NVENCSTATUS lockStatus = NV_ENC_SUCCESS;
        LockAndConsumeBitstream(pkt, lockErr, /*non_blocking=*/true, &lockStatus);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastProgress).count();

        const FlushDrainStep step = NextFlushDrainStep(lockStatus, elapsedMs, kFlushDrainBudgetMs);
        if (step == FlushDrainStep::Consume) {
            out_packets.push_back(std::move(pkt));
            ++i;
            lastProgress = std::chrono::steady_clock::now();
            continue;
        }
        if (step == FlushDrainStep::Retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        // AbortTimeout / AbortError: stop draining but do not fail — the caller
        // (which ignores this return) still pushes video-EOS and finalises the
        // file with whatever was already muxed, instead of wedging.
        out_error = step == FlushDrainStep::AbortTimeout
                        ? "Flush drain timed out — device not delivering buffered frames"
                        : (std::string("Flush drain stopped: ") + lockErr);
        break;
    }

    m_needMoreInputCount = 0;
    return true;
}

// ---------------------------------------------------------------------------
// FlushAsync (internal)
// ---------------------------------------------------------------------------

bool NvencEncoder::FlushAsync(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) {
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    eos.completionEvent = m_eosEvent;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eos);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncEncodePicture(EOS): ") + NvencStatusName(st);
        return false;
    }

    // Same anti-wedge guarantee as the sync drain (NextFlushDrainStep): a
    // healthy device signals each pending frame's event well under the
    // budget (progress resets the clock); a lost/hung one is abandoned after
    // the budget so the caller still finalises instead of wedging.
    constexpr double kFlushDrainBudgetMs = 2000.0;
    while (!m_pending.empty()) {
        const int32_t headOutIdx = m_pending.front().out_idx;
        if (headOutIdx < 0 || headOutIdx >= kMaxOutputResources) {
            out_error = "FlushAsync: pending frame has an invalid output-ring index";
            break;
        }
        EncodedVideoPacket pkt;
        std::string waitErr;
        const EventDrainStep step =
            WaitAndConsumeOneAsync(m_outputResources[headOutIdx].event, kFlushDrainBudgetMs, pkt, waitErr);
        if (step == EventDrainStep::Consume) {
            out_packets.push_back(std::move(pkt));
            continue;
        }
        // AbortTimeout / AbortError: stop draining but do not fail — the
        // caller still pushes video-EOS and finalises with whatever was
        // already muxed, instead of wedging (same contract as sync Flush).
        out_error = waitErr;
        break;
    }

    m_needMoreInputCount = 0;
    return true;
}

// ---------------------------------------------------------------------------
// UnregisterAllSlots
// ---------------------------------------------------------------------------

void NvencEncoder::UnregisterAllSlots() {
    for (auto& slot : m_slots) {
        if (slot.mapped && slot.mappedResource != nullptr) {
            m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
            slot.mappedResource = nullptr;
            slot.mapped = false;
        }
        slot.in_flight = false;

        if (slot.registeredResource != nullptr) {
            m_funcs.nvEncUnregisterResource(m_encoder, slot.registeredResource);
            slot.registeredResource = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------

void NvencEncoder::Destroy() {
    UnregisterAllSlots();

    // Teardown order: events unregistered+closed, then bitstream buffer(s),
    // then the encoder itself. DestroyOutputRing() is a no-op in sync mode
    // (m_outputResources/m_eosEvent were never populated).
    DestroyOutputRing();

    if (m_encoder && m_funcs.nvEncDestroyBitstreamBuffer && m_bitstreamBuffer) {
        m_funcs.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstreamBuffer);
        m_bitstreamBuffer = nullptr;
    }
    if (m_encoder && m_funcs.nvEncDestroyEncoder) {
        m_funcs.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
}

} // namespace recorder_core
