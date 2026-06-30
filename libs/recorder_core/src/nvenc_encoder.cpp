#include "nvenc_encoder.h"

#include <recorder_core/packet_types.h>

#include <cstdio>
#include <cstring>
#include <sstream>

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

// Required NVENC input buffer format for a given bit depth.
// 8-bit → NV12; 10-bit → P010 (semi-planar 16-bit, MSB-aligned 10-bit data).
static NV_ENC_BUFFER_FORMAT RequiredInputFormat(BitDepth depth) noexcept {
    return (depth == BitDepth::Bit10) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
}

static const char* BufferFormatName(NV_ENC_BUFFER_FORMAT fmt) noexcept {
    return (fmt == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? "NV_ENC_BUFFER_FORMAT_YUV420_10BIT"
                                                      : "NV_ENC_BUFFER_FORMAT_NV12";
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
                                bool have_caps, int w_min, int w_max, int h_min, int h_max, BitDepth bit_depth) {
    const bool isH264 = (codec_index == 1);
    const bool isHevc = (codec_index == 2);
    std::ostringstream oss;
    oss << "encodeGUID=" << GuidDebugString(p.encodeGUID) << ", presetGUID=" << GuidDebugString(p.presetGUID)
        << ", profileGUID=" << GuidDebugString(cfg.profileGUID) << ", tuningInfo=" << TuningInfoName(p.tuningInfo)
        << ", encodeWidth=" << p.encodeWidth << ", encodeHeight=" << p.encodeHeight << ", darWidth=" << p.darWidth
        << ", darHeight=" << p.darHeight << ", maxEncodeWidth=" << p.maxEncodeWidth
        << ", maxEncodeHeight=" << p.maxEncodeHeight << ", frameRateNum=" << p.frameRateNum
        << ", frameRateDen=" << p.frameRateDen << ", bufferFormat=" << BufferFormatName(RequiredInputFormat(bit_depth))
        << ", enablePTD=" << static_cast<int>(p.enablePTD)
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

    const NV_ENC_BUFFER_FORMAT wantFmt = RequiredInputFormat(m_bitDepth);
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

    const NV_ENC_BUFFER_FORMAT wantFmt = RequiredInputFormat(m_bitDepth);
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

RcParams ComputeNvencRcParams(RateControlMode mode, NvencQualityPreset quality, uint32_t bitrate_kbps) {
    RcParams p{};
    switch (mode) {
    case RateControlMode::ConstantQuality: {
        p.rateControlMode = static_cast<uint32_t>(NV_ENC_PARAMS_RC_CONSTQP);
        switch (quality) {
        case NvencQualityPreset::High:
            p.qpIntra = 19;
            p.qpInterP = 21;
            p.qpInterB = 21;
            break;
        case NvencQualityPreset::Balanced:
            p.qpIntra = 24;
            p.qpInterP = 26;
            p.qpInterB = 26;
            break;
        case NvencQualityPreset::Small:
            p.qpIntra = 30;
            p.qpInterP = 32;
            p.qpInterB = 32;
            break;
        }
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
        p = ComputeNvencRcParams(RateControlMode::ConstantQuality, NvencQualityPreset::Balanced, bitrate_kbps);
        break;
    }
    return p;
}

// ---------------------------------------------------------------------------
// FetchPresetConfig
// ---------------------------------------------------------------------------

bool NvencEncoder::FetchPresetConfig(std::string& out_error) {
    m_presetConfig = {};
    m_presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    m_presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    // AV1 with P6 preset has internal pipeline depth causing NEED_MORE_INPUT on every frame
    // even when lookahead is disabled. P4 avoids this and produces frames synchronously.
    // HEVC uses P4 as well (same pipeline-depth concern applies for higher presets).
    GUID codecGuid = NV_ENC_CODEC_AV1_GUID;
    if (m_codec == VideoCodec::H264Nvenc) {
        codecGuid = NV_ENC_CODEC_H264_GUID;
        // m_presetGuid stays at P6 (set in member initializer) for H.264
    } else if (m_codec == VideoCodec::HevcNvenc) {
        codecGuid = NV_ENC_CODEC_HEVC_GUID;
        m_presetGuid = NV_ENC_PRESET_P4_GUID;
    } else {
        // AV1: use P4
        m_presetGuid = NV_ENC_PRESET_P4_GUID;
    }
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

    if (m_codec == VideoCodec::H264Nvenc) {
        m_encodeConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 1; // YUV420/NV12
    } else if (m_codec == VideoCodec::HevcNvenc) {
        m_encodeConfig.encodeCodecConfig.hevcConfig.chromaFormatIDC = 1; // YUV420/NV12 or P010
        m_encodeConfig.encodeCodecConfig.hevcConfig.inputBitDepth = nvBitDepth;
        m_encodeConfig.encodeCodecConfig.hevcConfig.outputBitDepth = nvBitDepth;
        if (tenBit)
            m_encodeConfig.profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
    } else {
        m_encodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1; // YUV420/NV12 or P010
        m_encodeConfig.encodeCodecConfig.av1Config.inputBitDepth = nvBitDepth;
        m_encodeConfig.encodeCodecConfig.av1Config.outputBitDepth = nvBitDepth;
        // AV1 uses a single Main profile GUID for both 8- and 10-bit; the bit depth is
        // signaled by the input/output bit-depth fields above, not a distinct profile.
        if (tenBit)
            m_encodeConfig.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
    }

    // Apply canonical rate-control via the pure, testable ComputeNvencRcParams helper.
    // NVENC SDK field names: rcParams.rateControlMode / constQP / averageBitRate / maxBitRate.
    const RcParams rc = ComputeNvencRcParams(m_rateControlMode, m_qualityPreset, m_bitrate_kbps);
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

    return true;
}

// ---------------------------------------------------------------------------
// InitEncoder
// ---------------------------------------------------------------------------

bool NvencEncoder::InitEncoder(uint32_t width, uint32_t height, uint32_t frame_rate_num, uint32_t frame_rate_den,
                               std::string& out_error) {
    // Keyframe interval: gopLength = round(interval_secs * fps).
    // m_keyframeIntervalSecs defaults to 2.0 (pre-0.9.0 hardcoded behaviour).
    const uint32_t kGopFrames =
        (frame_rate_den > 0 && frame_rate_num > 0)
            ? static_cast<uint32_t>(m_keyframeIntervalSecs * static_cast<float>(frame_rate_num) /
                                        static_cast<float>(frame_rate_den) +
                                    0.5f)
            : 120u;
    m_encodeConfig.gopLength = kGopFrames;
    if (m_codec == VideoCodec::H264Nvenc) {
        m_encodeConfig.encodeCodecConfig.h264Config.idrPeriod = kGopFrames;
    } else if (m_codec == VideoCodec::HevcNvenc) {
        m_encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = kGopFrames;
    } else {
        m_encodeConfig.encodeCodecConfig.av1Config.idrPeriod = kGopFrames;
    }

    GUID codecGuid = NV_ENC_CODEC_AV1_GUID;
    if (m_codec == VideoCodec::H264Nvenc)
        codecGuid = NV_ENC_CODEC_H264_GUID;
    else if (m_codec == VideoCodec::HevcNvenc)
        codecGuid = NV_ENC_CODEC_HEVC_GUID;

    // codec_index: 0=AV1, 1=H264, 2=HEVC — used in diag string and error messages
    const int codec_index = (m_codec == VideoCodec::H264Nvenc) ? 1 : (m_codec == VideoCodec::HevcNvenc) ? 2 : 0;

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
    p.encodeConfig = &m_encodeConfig;

    const bool evenWidth = (width % 2u) == 0u;
    const bool evenHeight = (height % 2u) == 0u;
    const bool widthInRange =
        !haveCaps || (width >= static_cast<uint32_t>(capWidthMin) && width <= static_cast<uint32_t>(capWidthMax));
    const bool heightInRange =
        !haveCaps || (height >= static_cast<uint32_t>(capHeightMin) && height <= static_cast<uint32_t>(capHeightMax));

    const std::string initDiag = BuildInitDiagString(p, m_encodeConfig, codec_index, haveCaps, capWidthMin, capWidthMax,
                                                     capHeightMin, capHeightMax, m_bitDepth);

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
    reg.bufferFormat = RequiredInputFormat(m_bitDepth);
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

bool NvencEncoder::LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error) {
    NV_ENC_LOCK_BITSTREAM lockBS{};
    lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBS.outputBitstream = m_bitstreamBuffer;
    lockBS.doNotWait = 0;

    NVENCSTATUS st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncLockBitstream: ") + NvencStatusName(st);
        return false;
    }

    uint64_t ts_ns = 0;
    if (!m_pendingPts.empty()) {
        ts_ns = m_pendingPts.front();
        m_pendingPts.pop();
    }

    // Release the associated input slot
    if (!m_pendingSlots.empty()) {
        int32_t slot_idx = m_pendingSlots.front();
        m_pendingSlots.pop();

        if (slot_idx >= 0 && slot_idx < 8) {
            InputSlot& slot = m_slots[slot_idx];
            if (slot.mapped && slot.mappedResource != nullptr) {
                m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
                slot.mappedResource = nullptr;
            }
            slot.mapped = false;
            slot.in_flight = false;
        }
    }

    bool isKey = (lockBS.pictureType == NV_ENC_PIC_TYPE_IDR || lockBS.pictureType == NV_ENC_PIC_TYPE_I);

    out_packet.pts_ns = ts_ns;
    out_packet.keyframe = isKey;
    out_packet.bytes.assign(static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                            static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes);

    m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
    return true;
}

// ---------------------------------------------------------------------------
// EncodeFrame
// ---------------------------------------------------------------------------

bool NvencEncoder::EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                               EncodedVideoPacket* out_packet, std::string& out_error) {
    if (slot_idx < 0 || slot_idx >= 8) {
        out_error = "EncodeFrame: slot_idx out of range [0,7]";
        return false;
    }
    InputSlot& slot = m_slots[slot_idx];
    if (slot.mapped) {
        out_error = "EncodeFrame: slot already mapped";
        return false;
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

    // Push PTS and slot index before submission (FIFO: one entry per submitted frame)
    m_pendingPts.push(pts_ns);
    m_pendingSlots.push(slot_idx);

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.inputPitch = 0;
    pic.inputBuffer = mapRes.mappedResource;
    pic.outputBitstream = m_bitstreamBuffer;
    pic.bufferFmt = mapRes.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    if (m_forceIdrNext) {
        // Force an IDR at a segment boundary: the first frame of the new segment
        // must be a self-contained keyframe carrying fresh SPS/PPS so no dependent
        // frame precedes it. Consume the one-shot request.
        pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        m_forceIdrNext = false;
    }
    pic.inputTimeStamp = m_frameIdx++;

    st = m_funcs.nvEncEncodePicture(m_encoder, &pic);

    if (st == NV_ENC_SUCCESS) {
        // Output available immediately.
        // LockAndConsumeBitstream pops the earliest pending PTS+slot and
        // releases that slot (unmap + mark free).
        if (out_packet) {
            std::string lockErr;
            if (!LockAndConsumeBitstream(*out_packet, lockErr)) {
                // Bitstream lock failed: clean up the current slot.
                m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
                slot.mappedResource = nullptr;
                slot.mapped = false;
                slot.in_flight = false;

                // Pop our own PTS+slot entries; LockAndConsumeBitstream
                // may have already popped them if it got partway through.
                // Best-effort: drain matching pair if present.
                if (!m_pendingPts.empty() && !m_pendingSlots.empty()) {
                    m_pendingPts.pop();
                    m_pendingSlots.pop();
                }
                out_error = lockErr;
                return false;
            }
        }
        return true;
    } else if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        // Buffered — PTS and slot are queued; do not unmap.
        // The slot will be released later when its output is consumed.
        ++m_needMoreInputCount;
        if (out_packet) {
            out_packet->bytes.clear();
        }
        return true;
    } else {
        // Fatal encode error — clean up this slot
        m_funcs.nvEncUnmapInputResource(m_encoder, slot.mappedResource);
        slot.mappedResource = nullptr;
        slot.mapped = false;
        slot.in_flight = false;

        // Remove the PTS+slot we just pushed (best-effort, front of queue)
        if (!m_pendingPts.empty())
            m_pendingPts.pop();
        if (!m_pendingSlots.empty())
            m_pendingSlots.pop();

        out_error = std::string("nvEncEncodePicture: ") + NvencStatusName(st);
        return false;
    }
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

bool NvencEncoder::Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) {
    // Send EOS
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eos);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncEncodePicture(EOS): ") + NvencStatusName(st);
        return false;
    }

    // Drain buffered frames.
    // LockAndConsumeBitstream releases each slot after output is consumed.
    for (int i = 0; i < m_needMoreInputCount; ++i) {
        if (m_pendingPts.empty())
            break;

        EncodedVideoPacket pkt;
        std::string lockErr;
        if (!LockAndConsumeBitstream(pkt, lockErr)) {
            out_error = std::string("Flush drain stopped: ") + lockErr;
            break;
        }
        out_packets.push_back(std::move(pkt));
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
