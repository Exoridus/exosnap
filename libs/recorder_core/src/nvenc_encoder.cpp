#include "nvenc_encoder.h"

#include <recorder_core/packet_types.h>

#include <cstdio>
#include <cstring>

namespace recorder_core {

// ---------------------------------------------------------------------------
// NvencStatusName
// ---------------------------------------------------------------------------

const char* NvencStatusName(NVENCSTATUS st) noexcept {
    switch (st) {
    case NV_ENC_SUCCESS:                      return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:         return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:       return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:    return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:           return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:         return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR:              return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT:            return "NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM:            return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:             return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:            return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:  return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:        return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY:                return "NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER:        return "NV_ENC_ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION:          return "NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED:               return "NV_ENC_ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT:          return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:             return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD:      return "NV_ENC_ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC:                  return "NV_ENC_ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:  return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:            return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:  return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED:      return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
    default:                                  return "NV_ENC_ERR_UNKNOWN";
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
    auto pCreate = reinterpret_cast<PFN>(
        GetProcAddress(m_dll, "NvEncodeAPICreateInstance"));
    if (!pCreate) {
        out_error = "NvEncodeAPICreateInstance not exported from nvEncodeAPI64.dll";
        return false;
    }

    m_funcs         = {};
    m_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = pCreate(&m_funcs);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("NvEncodeAPICreateInstance: ") + NvencStatusName(st);
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device     = device;
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

    bool nv12Found = false;
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_NV12) { nv12Found = true; break; }
    }
    if (!nv12Found) {
        out_error = "NV_ENC_BUFFER_FORMAT_NV12 not in AV1 input formats";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// FetchPresetConfig
// ---------------------------------------------------------------------------

bool NvencEncoder::FetchPresetConfig(std::string& out_error) {
    m_presetConfig                   = {};
    m_presetConfig.version           = NV_ENC_PRESET_CONFIG_VER;
    m_presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = m_funcs.nvEncGetEncodePresetConfigEx(
        m_encoder, NV_ENC_CODEC_AV1_GUID, m_presetGuid, m_tuningInfo, &m_presetConfig);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncGetEncodePresetConfigEx: ") + NvencStatusName(st);
        return false;
    }

    std::memcpy(&m_encodeConfig, &m_presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    m_encodeConfig.version = NV_ENC_CONFIG_VER;
    m_encodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1; // YUV420/NV12

    // M3.2 committed low-latency config: zero lookahead and P-only (no B-frames).
    // Required to prevent the 8-slot NVENC input ring from exhausting — zero encoder
    // delay means each slot is released as soon as its output is consumed, keeping
    // slot acquisition always available for the next captured frame.
    m_encodeConfig.rcParams.enableLookahead = 0;
    m_encodeConfig.rcParams.lookaheadDepth = 0;
    m_encodeConfig.frameIntervalP = 1;

    return true;
}

// ---------------------------------------------------------------------------
// InitEncoder
// ---------------------------------------------------------------------------

bool NvencEncoder::InitEncoder(
    uint32_t width, uint32_t height,
    uint32_t frame_rate_num, uint32_t frame_rate_den,
    std::string& out_error)
{
    NV_ENC_INITIALIZE_PARAMS p{};
    p.version         = NV_ENC_INITIALIZE_PARAMS_VER;
    p.encodeGUID      = NV_ENC_CODEC_AV1_GUID;
    p.presetGUID      = m_presetGuid;
    p.tuningInfo      = m_tuningInfo;
    p.encodeWidth     = width;
    p.encodeHeight    = height;
    p.darWidth        = width;
    p.darHeight       = height;
    p.maxEncodeWidth  = width;
    p.maxEncodeHeight = height;
    p.frameRateNum    = frame_rate_num;
    p.frameRateDen    = frame_rate_den;
    p.enablePTD       = 1;
    p.encodeConfig    = &m_encodeConfig;

    NVENCSTATUS st = m_funcs.nvEncInitializeEncoder(m_encoder, &p);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncInitializeEncoder: ") + NvencStatusName(st);
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

bool NvencEncoder::RegisterSlotTexture(
    int32_t              slot_idx,
    ID3D11Texture2D*     texture,
    std::string&         out_error)
{
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
    reg.version            = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType       = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width              = desc.Width;
    reg.height             = desc.Height;
    reg.pitch              = 0;
    reg.subResourceIndex   = 0;
    reg.resourceToRegister = texture;
    reg.bufferFormat       = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage        = NV_ENC_INPUT_IMAGE;

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
            m_slotCursor   = (idx + 1) % 8;
            return idx;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// LockAndConsumeBitstream (internal)
// ---------------------------------------------------------------------------

bool NvencEncoder::LockAndConsumeBitstream(EncodedVideoPacket& out_packet, std::string& out_error) {
    NV_ENC_LOCK_BITSTREAM lockBS{};
    lockBS.version         = NV_ENC_LOCK_BITSTREAM_VER;
    lockBS.outputBitstream = m_bitstreamBuffer;
    lockBS.doNotWait       = 0;

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
            slot.mapped     = false;
            slot.in_flight  = false;
        }
    }

    bool isKey = (lockBS.pictureType == NV_ENC_PIC_TYPE_IDR
               || lockBS.pictureType == NV_ENC_PIC_TYPE_I);

    out_packet.pts_ns   = ts_ns;
    out_packet.keyframe = isKey;
    out_packet.bytes.assign(
        static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
        static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes);

    m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
    return true;
}

// ---------------------------------------------------------------------------
// EncodeFrame
// ---------------------------------------------------------------------------

bool NvencEncoder::EncodeFrame(
    int32_t             slot_idx,
    uint64_t            pts_ns,
    uint32_t            width,
    uint32_t            height,
    EncodedVideoPacket* out_packet,
    std::string&        out_error)
{
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
    mapRes.version            = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = slot.registeredResource;

    NVENCSTATUS st = m_funcs.nvEncMapInputResource(m_encoder, &mapRes);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncMapInputResource: ") + NvencStatusName(st);
        return false;
    }
    slot.mappedResource = mapRes.mappedResource;
    slot.mapped         = true;

    // Push PTS and slot index before submission (FIFO: one entry per submitted frame)
    m_pendingPts.push(pts_ns);
    m_pendingSlots.push(slot_idx);

    NV_ENC_PIC_PARAMS pic{};
    pic.version         = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth      = width;
    pic.inputHeight     = height;
    pic.inputPitch      = 0;
    pic.inputBuffer     = mapRes.mappedResource;
    pic.outputBitstream = m_bitstreamBuffer;
    pic.bufferFmt       = mapRes.mappedBufferFmt;
    pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
    pic.encodePicFlags  = 0;
    pic.inputTimeStamp  = m_frameIdx++;

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
                slot.mapped    = false;
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
        slot.mapped    = false;
        slot.in_flight = false;

        // Remove the PTS+slot we just pushed (best-effort, front of queue)
        if (!m_pendingPts.empty()) m_pendingPts.pop();
        if (!m_pendingSlots.empty()) m_pendingSlots.pop();

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
    eos.version        = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eos);
    if (st != NV_ENC_SUCCESS) {
        out_error = std::string("nvEncEncodePicture(EOS): ") + NvencStatusName(st);
        return false;
    }

    // Drain buffered frames.
    // LockAndConsumeBitstream releases each slot after output is consumed.
    for (int i = 0; i < m_needMoreInputCount; ++i) {
        if (m_pendingPts.empty()) break;

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
            slot.mapped    = false;
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
