#include "nvenc_probe.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace nvenc_probe {

namespace {

constexpr int kPadColumn = 42;

void PrintPhasePass(int phase, const char* label) {
    char prefix[80];
    int n = snprintf(prefix, sizeof(prefix), "[phase %02d] %s ", phase, label);
    if (n < 0) {
        n = 0;
    }
    fwrite(prefix, 1, static_cast<size_t>(n), stdout);
    for (int i = n; i < kPadColumn; ++i) {
        fputc('.', stdout);
    }
    fprintf(stdout, " PASS\n");
    fflush(stdout);
}

void PrintPhaseFail(int phase, const char* label, const char* reason) {
    char prefix[80];
    int n = snprintf(prefix, sizeof(prefix), "[phase %02d] %s ", phase, label);
    if (n < 0) {
        n = 0;
    }
    fwrite(prefix, 1, static_cast<size_t>(n), stdout);
    for (int i = n; i < kPadColumn; ++i) {
        fputc('.', stdout);
    }
    fprintf(stdout, " FAIL: %s\n", reason);
    fflush(stdout);
}

void PrintPhaseSkip(int phase, const char* label) {
    char prefix[80];
    int n = snprintf(prefix, sizeof(prefix), "[phase %02d] %s ", phase, label);
    if (n < 0) {
        n = 0;
    }
    fwrite(prefix, 1, static_cast<size_t>(n), stdout);
    for (int i = n; i < kPadColumn; ++i) {
        fputc('.', stdout);
    }
    fprintf(stdout, " SKIP\n");
    fflush(stdout);
}

void PrintDetail(const char* line) {
    fprintf(stdout, "           %s\n", line);
    fflush(stdout);
}

} // namespace

const char* NvencStatusName(NVENCSTATUS st) {
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

BaselineProbe::BaselineProbe() = default;

BaselineProbe::~BaselineProbe() {
    Cleanup();
}

void BaselineProbe::CleanupEncoder() {
    if (m_encoder != nullptr && m_funcs.nvEncDestroyInputBuffer != nullptr && m_inputBuffer != nullptr) {
        m_funcs.nvEncDestroyInputBuffer(m_encoder, m_inputBuffer);
        m_inputBuffer = nullptr;
    }
    if (m_encoder != nullptr && m_funcs.nvEncDestroyBitstreamBuffer != nullptr && m_bitstreamBuffer != nullptr) {
        m_funcs.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstreamBuffer);
        m_bitstreamBuffer = nullptr;
    }
    if (m_encoder != nullptr && m_funcs.nvEncDestroyEncoder != nullptr) {
        m_funcs.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
}

void BaselineProbe::Cleanup() {
    CleanupEncoder();
    m_d3dContext = nullptr;
    m_d3dDevice = nullptr;
    if (m_dll != nullptr) {
        FreeLibrary(m_dll);
        m_dll = nullptr;
    }
}

bool BaselineProbe::Phase01_LoadDll() {
    m_dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (m_dll == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LoadLibraryW returned NULL (GetLastError=%lu)",
                 static_cast<unsigned long>(GetLastError()));
        PrintPhaseFail(1, "load nvEncodeAPI64.dll", buf);
        return false;
    }
    PrintPhasePass(1, "load nvEncodeAPI64.dll");
    return true;
}

bool BaselineProbe::Phase02_CreateApiInstance() {
    using PFN_NvEncodeAPICreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<PFN_NvEncodeAPICreateInstance>(GetProcAddress(m_dll, "NvEncodeAPICreateInstance"));
    if (pCreate == nullptr) {
        PrintPhaseFail(2, "create NVENC API instance", "NvEncodeAPICreateInstance not exported");
        return false;
    }
    m_funcs = {};
    m_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = pCreate(&m_funcs);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(2, "create NVENC API instance", NvencStatusName(st));
        return false;
    }
    PrintPhasePass(2, "create NVENC API instance");
    char detail[96];
    snprintf(detail, sizeof(detail), "client header NVENCAPI_VERSION major=%u minor=%u",
             static_cast<unsigned>(NVENCAPI_MAJOR_VERSION), static_cast<unsigned>(NVENCAPI_MINOR_VERSION));
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase03_CreateD3D11Device() {
    winrt::com_ptr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
    if (FAILED(hr) || factory == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "CreateDXGIFactory1 failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(3, "create D3D11 device", buf);
        return false;
    }

    winrt::com_ptr<IDXGIAdapter1> chosenAdapter;
    DXGI_ADAPTER_DESC1 chosenDesc{};

    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        if (desc.VendorId == 0x10DE) {
            chosenAdapter = adapter;
            chosenDesc = desc;
            break;
        }
        if (chosenAdapter == nullptr) {
            chosenAdapter = adapter;
            chosenDesc = desc;
        }
    }

    if (chosenAdapter == nullptr) {
        PrintPhaseFail(3, "create D3D11 device", "no hardware DXGI adapter found");
        return false;
    }

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
    hr = D3D11CreateDevice(chosenAdapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevel, 1,
                           D3D11_SDK_VERSION, m_d3dDevice.put(), nullptr, m_d3dContext.put());
    if (FAILED(hr) || m_d3dDevice == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(3, "create D3D11 device", buf);
        return false;
    }

    m_adapterName = chosenDesc.Description;
    m_adapterVendorId = chosenDesc.VendorId;

    PrintPhasePass(3, "create D3D11 device");
    int len = WideCharToMultiByte(CP_UTF8, 0, m_adapterName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Name;
    if (len > 1) {
        utf8Name.resize(static_cast<size_t>(len - 1));
        WideCharToMultiByte(CP_UTF8, 0, m_adapterName.c_str(), -1, utf8Name.data(), len, nullptr, nullptr);
    }
    char detail[256];
    snprintf(detail, sizeof(detail), "adapter: %s (vendor 0x%04X)", utf8Name.c_str(),
             static_cast<unsigned>(m_adapterVendorId));
    PrintDetail(detail);
    if (m_adapterVendorId != 0x10DE) {
        PrintDetail("warning: chosen adapter is not an NVIDIA device (vendor != 0x10DE)");
    }
    return true;
}

bool BaselineProbe::Phase04_OpenEncodeSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = m_d3dDevice.get();
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_funcs.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS || m_encoder == nullptr) {
        PrintPhaseFail(4, "open encode session", NvencStatusName(st));
        return false;
    }
    PrintPhasePass(4, "open encode session");
    return true;
}

bool BaselineProbe::Phase05_QueryH264Support() {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetEncodeGUIDCount(m_encoder, &count);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(5, "H.264 GUID support", NvencStatusName(st));
        return false;
    }
    if (count == 0) {
        PrintPhaseFail(5, "H.264 GUID support", "encoder reports zero supported encode GUIDs");
        return false;
    }
    std::vector<GUID> guids(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetEncodeGUIDs(m_encoder, guids.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(5, "H.264 GUID support", NvencStatusName(st));
        return false;
    }
    for (uint32_t i = 0; i < got; ++i) {
        if (IsEqualGUID(guids[i], NV_ENC_CODEC_H264_GUID) != 0) {
            PrintPhasePass(5, "H.264 GUID support");
            return true;
        }
    }
    PrintPhaseFail(5, "H.264 GUID support", "NV_ENC_CODEC_H264_GUID not in supported encode GUIDs");
    return false;
}

bool BaselineProbe::Phase06_QueryNv12Support() {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_H264_GUID, &count);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(6, "NV12 input format support", NvencStatusName(st));
        return false;
    }
    if (count == 0) {
        PrintPhaseFail(6, "NV12 input format support", "encoder reports zero input formats for H.264");
        return false;
    }
    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_H264_GUID, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(6, "NV12 input format support", NvencStatusName(st));
        return false;
    }
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_NV12) {
            PrintPhasePass(6, "NV12 input format support");
            char detail[64];
            snprintf(detail, sizeof(detail), "chosen input format: NV_ENC_BUFFER_FORMAT_NV12");
            PrintDetail(detail);
            return true;
        }
    }
    PrintPhaseFail(6, "NV12 input format support", "NV_ENC_BUFFER_FORMAT_NV12 not in supported input formats");
    return false;
}

bool BaselineProbe::Phase07_FetchPresetConfig() {
    m_presetConfig = {};
    m_presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    m_presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = m_funcs.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_H264_GUID, m_presetGuid, m_tuningInfo,
                                                          &m_presetConfig);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(7, "fetch preset config", NvencStatusName(st));
        return false;
    }

    std::memcpy(&m_encodeConfig, &m_presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    m_encodeConfig.version = NV_ENC_CONFIG_VER;

    PrintPhasePass(7, "fetch preset config");
    PrintDetail("preset: NV_ENC_PRESET_P4_GUID, tuning: NV_ENC_TUNING_INFO_HIGH_QUALITY");
    return true;
}

bool BaselineProbe::Phase08_InitializeEncoder() {
    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = m_presetGuid;
    initParams.tuningInfo = m_tuningInfo;
    initParams.encodeWidth = m_encodeWidth;
    initParams.encodeHeight = m_encodeHeight;
    initParams.darWidth = m_encodeWidth;
    initParams.darHeight = m_encodeHeight;
    initParams.maxEncodeWidth = m_encodeWidth;
    initParams.maxEncodeHeight = m_encodeHeight;
    initParams.frameRateNum = m_frameRateNum;
    initParams.frameRateDen = m_frameRateDen;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &m_encodeConfig;

    NVENCSTATUS st = m_funcs.nvEncInitializeEncoder(m_encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(8, "H.264 baseline init", NvencStatusName(st));
        return false;
    }
    PrintPhasePass(8, "H.264 baseline init");
    char detail[160];
    snprintf(detail, sizeof(detail), "%ux%u @ %u/%u fps, preset P4, tuning HIGH_QUALITY, enablePTD=1",
             static_cast<unsigned>(m_encodeWidth), static_cast<unsigned>(m_encodeHeight),
             static_cast<unsigned>(m_frameRateNum), static_cast<unsigned>(m_frameRateDen));
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase09_CreateBuffers() {
    NV_ENC_CREATE_INPUT_BUFFER inputBufParams{};
    inputBufParams.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inputBufParams.width = m_encodeWidth;
    inputBufParams.height = m_encodeHeight;
    inputBufParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS st = m_funcs.nvEncCreateInputBuffer(m_encoder, &inputBufParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(9, "create NVENC input/output buffers", NvencStatusName(st));
        return false;
    }
    m_inputBuffer = inputBufParams.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER bsBufParams{};
    bsBufParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    st = m_funcs.nvEncCreateBitstreamBuffer(m_encoder, &bsBufParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(9, "create NVENC input/output buffers", NvencStatusName(st));
        return false;
    }
    m_bitstreamBuffer = bsBufParams.bitstreamBuffer;

    PrintPhasePass(9, "create NVENC input/output buffers");
    return true;
}

bool BaselineProbe::Phase10_FillFrame() {
    NV_ENC_LOCK_INPUT_BUFFER lockParams{};
    lockParams.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lockParams.inputBuffer = m_inputBuffer;
    lockParams.doNotWait = 0;

    NVENCSTATUS st = m_funcs.nvEncLockInputBuffer(m_encoder, &lockParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(10, "fill one NV12 frame", NvencStatusName(st));
        return false;
    }

    m_inputPitch = lockParams.pitch;

    auto* data = static_cast<uint8_t*>(lockParams.bufferDataPtr);
    // Y-plane: height rows of pitch bytes, all 128
    std::memset(data, 128, static_cast<size_t>(m_inputPitch) * m_encodeHeight);
    // UV-plane: height/2 rows of pitch bytes (interleaved U and V), all 128
    std::memset(data + static_cast<size_t>(m_inputPitch) * m_encodeHeight, 128,
                static_cast<size_t>(m_inputPitch) * (m_encodeHeight / 2));

    st = m_funcs.nvEncUnlockInputBuffer(m_encoder, m_inputBuffer);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(10, "fill one NV12 frame", NvencStatusName(st));
        return false;
    }

    PrintPhasePass(10, "fill one NV12 frame");
    char detail[80];
    snprintf(detail, sizeof(detail), "pitch=%u, Y=128 U=128 V=128 (50%% gray, deterministic)", m_inputPitch);
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase11_SubmitFrame() {
    NV_ENC_PIC_PARAMS picParams{};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputWidth = m_encodeWidth;
    picParams.inputHeight = m_encodeHeight;
    picParams.inputPitch = (m_inputPitch != 0) ? m_inputPitch : m_encodeWidth;
    picParams.inputBuffer = m_inputBuffer;
    picParams.outputBitstream = m_bitstreamBuffer;
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.encodePicFlags = 0;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &picParams);
    if (st == NV_ENC_SUCCESS) {
        m_needMoreInput = false;
        PrintPhasePass(11, "submit one H.264 frame");
        return true;
    }
    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        m_needMoreInput = true;
        PrintPhasePass(11, "submit one H.264 frame");
        PrintDetail("NV_ENC_ERR_NEED_MORE_INPUT: encoder buffered frame, will verify after EOS flush");
        return true;
    }
    PrintPhaseFail(11, "submit one H.264 frame", NvencStatusName(st));
    return false;
}

bool BaselineProbe::Phase12_LockBitstream() {
    if (m_needMoreInput) {
        PrintPhaseSkip(12, "lock bitstream / verify bytes > 0");
        PrintDetail("encoder requested more input; verifying after EOS flush");
        return true;
    }

    NV_ENC_LOCK_BITSTREAM lockBS{};
    lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBS.outputBitstream = m_bitstreamBuffer;
    lockBS.doNotWait = 0;

    NVENCSTATUS st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(12, "lock bitstream / verify bytes > 0", NvencStatusName(st));
        return false;
    }

    uint32_t sizeInBytes = lockBS.bitstreamSizeInBytes;
    uint32_t picType = static_cast<uint32_t>(lockBS.pictureType);

    NVENCSTATUS unlockSt = m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);

    if (sizeInBytes == 0) {
        PrintPhaseFail(12, "lock bitstream / verify bytes > 0", "bitstreamSizeInBytes == 0");
        return false;
    }
    if (unlockSt != NV_ENC_SUCCESS) {
        PrintPhaseFail(12, "lock bitstream / verify bytes > 0", NvencStatusName(unlockSt));
        return false;
    }

    PrintPhasePass(12, "lock bitstream / verify bytes > 0");
    char detail[96];
    snprintf(detail, sizeof(detail), "bitstreamSizeInBytes=%u, pictureType=%u", sizeInBytes, picType);
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase13_FlushAndCleanup() {
    NV_ENC_PIC_PARAMS eosParams{};
    eosParams.version = NV_ENC_PIC_PARAMS_VER;
    eosParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eosParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(13, "EOS flush and cleanup", NvencStatusName(st));
        return false;
    }

    if (m_needMoreInput) {
        NV_ENC_LOCK_BITSTREAM lockBS{};
        lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBS.outputBitstream = m_bitstreamBuffer;
        lockBS.doNotWait = 0;

        st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
        if (st != NV_ENC_SUCCESS) {
            PrintPhaseFail(13, "EOS flush and cleanup", NvencStatusName(st));
            return false;
        }

        uint32_t sizeInBytes = lockBS.bitstreamSizeInBytes;
        uint32_t picType = static_cast<uint32_t>(lockBS.pictureType);

        NVENCSTATUS unlockSt = m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);

        if (sizeInBytes == 0) {
            PrintPhaseFail(13, "EOS flush and cleanup", "post-EOS bitstreamSizeInBytes == 0");
            return false;
        }
        if (unlockSt != NV_ENC_SUCCESS) {
            PrintPhaseFail(13, "EOS flush and cleanup", NvencStatusName(unlockSt));
            return false;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "post-EOS bitstreamSizeInBytes=%u, pictureType=%u",
                 sizeInBytes, picType);
        PrintDetail(detail);
    }

    PrintPhasePass(13, "EOS flush and cleanup");
    return true;
}

bool BaselineProbe::Phase14_OpenAv1Session() {
    CleanupEncoder();
    m_needMoreInput = false;
    m_inputPitch = 0;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = m_d3dDevice.get();
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_funcs.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS || m_encoder == nullptr) {
        PrintPhaseFail(14, "open AV1 encode session", NvencStatusName(st));
        return false;
    }
    PrintPhasePass(14, "open AV1 encode session");
    return true;
}

bool BaselineProbe::Phase15_QueryAv1Support() {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetEncodeGUIDCount(m_encoder, &count);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(15, "AV1 GUID support", NvencStatusName(st));
        return false;
    }
    if (count == 0) {
        PrintPhaseFail(15, "AV1 GUID support", "encoder reports zero supported encode GUIDs");
        return false;
    }
    std::vector<GUID> guids(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetEncodeGUIDs(m_encoder, guids.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(15, "AV1 GUID support", NvencStatusName(st));
        return false;
    }
    for (uint32_t i = 0; i < got; ++i) {
        if (IsEqualGUID(guids[i], NV_ENC_CODEC_AV1_GUID) != 0) {
            PrintPhasePass(15, "AV1 GUID support");
            return true;
        }
    }
    PrintPhaseFail(15, "AV1 GUID support",
                   "NV_ENC_CODEC_AV1_GUID not in supported encode GUIDs"
                   " (hardware/driver: Ada Lovelace / RTX 40-series or newer required)");
    return false;
}

bool BaselineProbe::Phase16_QueryAv1Nv12Format() {
    uint32_t count = 0;
    NVENCSTATUS st = m_funcs.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_AV1_GUID, &count);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(16, "AV1 NV12 input format", NvencStatusName(st));
        return false;
    }
    if (count == 0) {
        PrintPhaseFail(16, "AV1 NV12 input format", "encoder reports zero input formats for AV1");
        return false;
    }
    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    uint32_t got = 0;
    st = m_funcs.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_AV1_GUID, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(16, "AV1 NV12 input format", NvencStatusName(st));
        return false;
    }
    for (uint32_t i = 0; i < got; ++i) {
        if (fmts[i] == NV_ENC_BUFFER_FORMAT_NV12) {
            PrintPhasePass(16, "AV1 NV12 input format");
            PrintDetail("chosen input format: NV_ENC_BUFFER_FORMAT_NV12");
            return true;
        }
    }
    PrintPhaseFail(16, "AV1 NV12 input format",
                   "NV_ENC_BUFFER_FORMAT_NV12 not in supported AV1 input formats");
    return false;
}

bool BaselineProbe::Phase17_FetchAv1PresetConfig() {
    m_av1PresetConfig = {};
    m_av1PresetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    m_av1PresetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = m_funcs.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_AV1_GUID,
                                                          m_presetGuid, m_tuningInfo,
                                                          &m_av1PresetConfig);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(17, "fetch AV1 preset config", NvencStatusName(st));
        return false;
    }

    std::memcpy(&m_av1EncodeConfig, &m_av1PresetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    m_av1EncodeConfig.version = NV_ENC_CONFIG_VER;
    m_av1EncodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1;

    PrintPhasePass(17, "fetch AV1 preset config");
    PrintDetail("preset: NV_ENC_PRESET_P4_GUID, tuning: NV_ENC_TUNING_INFO_HIGH_QUALITY");
    return true;
}

bool BaselineProbe::Phase18_InitAv1Encoder() {
    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_AV1_GUID;
    initParams.presetGUID = m_presetGuid;
    initParams.tuningInfo = m_tuningInfo;
    initParams.encodeWidth = m_encodeWidth;
    initParams.encodeHeight = m_encodeHeight;
    initParams.darWidth = m_encodeWidth;
    initParams.darHeight = m_encodeHeight;
    initParams.maxEncodeWidth = m_encodeWidth;
    initParams.maxEncodeHeight = m_encodeHeight;
    initParams.frameRateNum = m_frameRateNum;
    initParams.frameRateDen = m_frameRateDen;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &m_av1EncodeConfig;

    NVENCSTATUS st = m_funcs.nvEncInitializeEncoder(m_encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(18, "AV1 encoder init", NvencStatusName(st));
        return false;
    }
    PrintPhasePass(18, "AV1 encoder init");
    char detail[160];
    snprintf(detail, sizeof(detail), "%ux%u @ %u/%u fps, AV1, preset P4, tuning HIGH_QUALITY, enablePTD=1",
             static_cast<unsigned>(m_encodeWidth), static_cast<unsigned>(m_encodeHeight),
             static_cast<unsigned>(m_frameRateNum), static_cast<unsigned>(m_frameRateDen));
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase19_CreateAv1Buffers() {
    NV_ENC_CREATE_INPUT_BUFFER inputBufParams{};
    inputBufParams.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inputBufParams.width = m_encodeWidth;
    inputBufParams.height = m_encodeHeight;
    inputBufParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS st = m_funcs.nvEncCreateInputBuffer(m_encoder, &inputBufParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(19, "create AV1 input/output buffers", NvencStatusName(st));
        return false;
    }
    m_inputBuffer = inputBufParams.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER bsBufParams{};
    bsBufParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    st = m_funcs.nvEncCreateBitstreamBuffer(m_encoder, &bsBufParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(19, "create AV1 input/output buffers", NvencStatusName(st));
        return false;
    }
    m_bitstreamBuffer = bsBufParams.bitstreamBuffer;

    PrintPhasePass(19, "create AV1 input/output buffers");
    return true;
}

bool BaselineProbe::Phase20_FillAv1Frame() {
    NV_ENC_LOCK_INPUT_BUFFER lockParams{};
    lockParams.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lockParams.inputBuffer = m_inputBuffer;
    lockParams.doNotWait = 0;

    NVENCSTATUS st = m_funcs.nvEncLockInputBuffer(m_encoder, &lockParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(20, "fill one NV12 frame", NvencStatusName(st));
        return false;
    }

    m_inputPitch = lockParams.pitch;

    auto* data = static_cast<uint8_t*>(lockParams.bufferDataPtr);
    std::memset(data, 128, static_cast<size_t>(m_inputPitch) * m_encodeHeight);
    std::memset(data + static_cast<size_t>(m_inputPitch) * m_encodeHeight, 128,
                static_cast<size_t>(m_inputPitch) * (m_encodeHeight / 2));

    st = m_funcs.nvEncUnlockInputBuffer(m_encoder, m_inputBuffer);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(20, "fill one NV12 frame", NvencStatusName(st));
        return false;
    }

    PrintPhasePass(20, "fill one NV12 frame");
    char detail[80];
    snprintf(detail, sizeof(detail), "pitch=%u, Y=128 U=128 V=128 (50%% gray, deterministic)", m_inputPitch);
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase21_SubmitAv1Frame() {
    NV_ENC_PIC_PARAMS picParams{};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputWidth = m_encodeWidth;
    picParams.inputHeight = m_encodeHeight;
    picParams.inputPitch = (m_inputPitch != 0) ? m_inputPitch : m_encodeWidth;
    picParams.inputBuffer = m_inputBuffer;
    picParams.outputBitstream = m_bitstreamBuffer;
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.encodePicFlags = 0;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &picParams);
    if (st == NV_ENC_SUCCESS) {
        m_needMoreInput = false;
        PrintPhasePass(21, "submit one AV1 frame");
        return true;
    }
    if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
        m_needMoreInput = true;
        PrintPhasePass(21, "submit one AV1 frame");
        PrintDetail("NV_ENC_ERR_NEED_MORE_INPUT: encoder buffered frame, will verify after EOS flush");
        return true;
    }
    PrintPhaseFail(21, "submit one AV1 frame", NvencStatusName(st));
    return false;
}

bool BaselineProbe::Phase22_LockAv1Bitstream() {
    if (m_needMoreInput) {
        PrintPhaseSkip(22, "lock AV1 bitstream / verify bytes > 0");
        PrintDetail("encoder requested more input; verifying after EOS flush");
        return true;
    }

    NV_ENC_LOCK_BITSTREAM lockBS{};
    lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBS.outputBitstream = m_bitstreamBuffer;
    lockBS.doNotWait = 0;

    NVENCSTATUS st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(22, "lock AV1 bitstream / verify bytes > 0", NvencStatusName(st));
        return false;
    }

    uint32_t sizeInBytes = lockBS.bitstreamSizeInBytes;
    uint32_t picType = static_cast<uint32_t>(lockBS.pictureType);

    NVENCSTATUS unlockSt = m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);

    if (sizeInBytes == 0) {
        PrintPhaseFail(22, "lock AV1 bitstream / verify bytes > 0", "bitstreamSizeInBytes == 0");
        return false;
    }
    if (unlockSt != NV_ENC_SUCCESS) {
        PrintPhaseFail(22, "lock AV1 bitstream / verify bytes > 0", NvencStatusName(unlockSt));
        return false;
    }

    PrintPhasePass(22, "lock AV1 bitstream / verify bytes > 0");
    char detail[96];
    snprintf(detail, sizeof(detail), "bitstreamSizeInBytes=%u, pictureType=%u", sizeInBytes, picType);
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase23_Av1FlushAndCleanup() {
    NV_ENC_PIC_PARAMS eosParams{};
    eosParams.version = NV_ENC_PIC_PARAMS_VER;
    eosParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eosParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(23, "AV1 EOS flush and cleanup", NvencStatusName(st));
        return false;
    }

    if (m_needMoreInput) {
        NV_ENC_LOCK_BITSTREAM lockBS{};
        lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBS.outputBitstream = m_bitstreamBuffer;
        lockBS.doNotWait = 0;

        st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
        if (st != NV_ENC_SUCCESS) {
            PrintPhaseFail(23, "AV1 EOS flush and cleanup", NvencStatusName(st));
            return false;
        }

        uint32_t sizeInBytes = lockBS.bitstreamSizeInBytes;
        uint32_t picType = static_cast<uint32_t>(lockBS.pictureType);

        NVENCSTATUS unlockSt = m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);

        if (sizeInBytes == 0) {
            PrintPhaseFail(23, "AV1 EOS flush and cleanup", "post-EOS bitstreamSizeInBytes == 0");
            return false;
        }
        if (unlockSt != NV_ENC_SUCCESS) {
            PrintPhaseFail(23, "AV1 EOS flush and cleanup", NvencStatusName(unlockSt));
            return false;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "post-EOS bitstreamSizeInBytes=%u, pictureType=%u",
                 sizeInBytes, picType);
        PrintDetail(detail);
    }

    PrintPhasePass(23, "AV1 EOS flush and cleanup");
    return true;
}

bool BaselineProbe::RunAv1Validation() {
    fprintf(stdout, "[probe] === AV1 one-frame validation ===\n");
    fflush(stdout);

    struct Phase {
        const char* name;
        bool (BaselineProbe::*fn)();
    };
    const Phase phases[] = {
        {"open AV1 encode session",               &BaselineProbe::Phase14_OpenAv1Session},
        {"AV1 GUID support",                      &BaselineProbe::Phase15_QueryAv1Support},
        {"AV1 NV12 input format",                 &BaselineProbe::Phase16_QueryAv1Nv12Format},
        {"fetch AV1 preset config",               &BaselineProbe::Phase17_FetchAv1PresetConfig},
        {"AV1 encoder init",                      &BaselineProbe::Phase18_InitAv1Encoder},
        {"create AV1 input/output buffers",       &BaselineProbe::Phase19_CreateAv1Buffers},
        {"fill one NV12 frame",                   &BaselineProbe::Phase20_FillAv1Frame},
        {"submit one AV1 frame",                  &BaselineProbe::Phase21_SubmitAv1Frame},
        {"lock AV1 bitstream / verify bytes > 0", &BaselineProbe::Phase22_LockAv1Bitstream},
        {"AV1 EOS flush and cleanup",             &BaselineProbe::Phase23_Av1FlushAndCleanup},
    };
    constexpr int kTotalPhases = sizeof(phases) / sizeof(phases[0]);

    for (int i = 0; i < kTotalPhases; ++i) {
        if (!(this->*(phases[i].fn))()) {
            fprintf(stdout, "STOP: AV1 validation did not pass at %s.\n", phases[i].name);
            fflush(stdout);
            return false;
        }
    }
    return true;
}

bool BaselineProbe::Phase24_OpenAv1SustainedSession() {
    m_sv_framesSubmitted    = 0;
    m_sv_packetsLocked      = 0;
    m_sv_needMoreInputCount = 0;
    m_sv_totalBytes         = 0;
    m_sv_elapsedSeconds     = 0.0;
    m_sv_packets.clear();

    CleanupEncoder();
    m_needMoreInput = false;
    m_inputPitch    = 0;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device     = m_d3dDevice.get();
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_funcs.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS || m_encoder == nullptr) {
        PrintPhaseFail(24, "open AV1 sustained session", NvencStatusName(st));
        return false;
    }

    m_av1PresetConfig = {};
    m_av1PresetConfig.version            = NV_ENC_PRESET_CONFIG_VER;
    m_av1PresetConfig.presetCfg.version  = NV_ENC_CONFIG_VER;
    st = m_funcs.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_AV1_GUID,
                                              m_presetGuid, m_tuningInfo,
                                              &m_av1PresetConfig);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(24, "open AV1 sustained session", NvencStatusName(st));
        return false;
    }
    std::memcpy(&m_av1EncodeConfig, &m_av1PresetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    m_av1EncodeConfig.version = NV_ENC_CONFIG_VER;
    m_av1EncodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1;

    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version         = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID      = NV_ENC_CODEC_AV1_GUID;
    initParams.presetGUID      = m_presetGuid;
    initParams.tuningInfo      = m_tuningInfo;
    initParams.encodeWidth     = m_encodeWidth;
    initParams.encodeHeight    = m_encodeHeight;
    initParams.darWidth        = m_encodeWidth;
    initParams.darHeight       = m_encodeHeight;
    initParams.maxEncodeWidth  = m_encodeWidth;
    initParams.maxEncodeHeight = m_encodeHeight;
    initParams.frameRateNum    = m_frameRateNum;
    initParams.frameRateDen    = m_frameRateDen;
    initParams.enablePTD       = 1;
    initParams.encodeConfig    = &m_av1EncodeConfig;

    st = m_funcs.nvEncInitializeEncoder(m_encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(24, "open AV1 sustained session", NvencStatusName(st));
        return false;
    }

    NV_ENC_CREATE_INPUT_BUFFER inputBufParams{};
    inputBufParams.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inputBufParams.width     = m_encodeWidth;
    inputBufParams.height    = m_encodeHeight;
    inputBufParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    st = m_funcs.nvEncCreateInputBuffer(m_encoder, &inputBufParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(24, "open AV1 sustained session", NvencStatusName(st));
        return false;
    }
    m_inputBuffer = inputBufParams.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER bsBufParams{};
    bsBufParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = m_funcs.nvEncCreateBitstreamBuffer(m_encoder, &bsBufParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(24, "open AV1 sustained session", NvencStatusName(st));
        return false;
    }
    m_bitstreamBuffer = bsBufParams.bitstreamBuffer;

    PrintPhasePass(24, "open AV1 sustained session");
    PrintDetail("AV1 sustained session ready: 1920x1080 @ 60/1 fps, preset P4, enablePTD=1");
    return true;
}

bool BaselineProbe::Phase25_Av1SustainedEncodeLoop() {
    constexpr int kFrameCount = 300;

    LARGE_INTEGER freq, tStart, tEnd;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    for (int frameIdx = 0; frameIdx < kFrameCount; ++frameIdx) {
        NV_ENC_LOCK_INPUT_BUFFER lockIn{};
        lockIn.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockIn.inputBuffer = m_inputBuffer;
        lockIn.doNotWait   = 0;

        NVENCSTATUS st = m_funcs.nvEncLockInputBuffer(m_encoder, &lockIn);
        if (st != NV_ENC_SUCCESS) {
            char buf[96];
            snprintf(buf, sizeof(buf), "frame %d: lock input: %s", frameIdx, NvencStatusName(st));
            PrintPhaseFail(25, "AV1 300-frame encode loop", buf);
            return false;
        }

        if (frameIdx == 0) {
            m_inputPitch = lockIn.pitch;
        }

        auto* data = static_cast<uint8_t*>(lockIn.bufferDataPtr);
        uint8_t yVal = static_cast<uint8_t>(frameIdx % 256);
        std::memset(data, yVal, static_cast<size_t>(m_inputPitch) * m_encodeHeight);
        std::memset(data + static_cast<size_t>(m_inputPitch) * m_encodeHeight, 128,
                    static_cast<size_t>(m_inputPitch) * (m_encodeHeight / 2));

        st = m_funcs.nvEncUnlockInputBuffer(m_encoder, m_inputBuffer);
        if (st != NV_ENC_SUCCESS) {
            char buf[96];
            snprintf(buf, sizeof(buf), "frame %d: unlock input: %s", frameIdx, NvencStatusName(st));
            PrintPhaseFail(25, "AV1 300-frame encode loop", buf);
            return false;
        }

        NV_ENC_PIC_PARAMS picParams{};
        picParams.version         = NV_ENC_PIC_PARAMS_VER;
        picParams.inputWidth      = m_encodeWidth;
        picParams.inputHeight     = m_encodeHeight;
        picParams.inputPitch      = (m_inputPitch != 0) ? m_inputPitch : m_encodeWidth;
        picParams.inputBuffer     = m_inputBuffer;
        picParams.outputBitstream = m_bitstreamBuffer;
        picParams.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
        picParams.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
        picParams.encodePicFlags  = 0;
        picParams.inputTimeStamp  = static_cast<uint64_t>(frameIdx);

        st = m_funcs.nvEncEncodePicture(m_encoder, &picParams);

        if (st == NV_ENC_SUCCESS) {
            m_sv_framesSubmitted++;

            NV_ENC_LOCK_BITSTREAM lockBS{};
            lockBS.version         = NV_ENC_LOCK_BITSTREAM_VER;
            lockBS.outputBitstream = m_bitstreamBuffer;
            lockBS.doNotWait       = 0;

            st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
            if (st != NV_ENC_SUCCESS) {
                char buf[96];
                snprintf(buf, sizeof(buf), "frame %d: lock bitstream: %s", frameIdx, NvencStatusName(st));
                PrintPhaseFail(25, "AV1 300-frame encode loop", buf);
                return false;
            }

            m_sv_totalBytes += lockBS.bitstreamSizeInBytes;
            m_sv_packetsLocked++;

            {
                Av1SustainedPacket pkt;
                pkt.timestamp = lockBS.outputTimeStamp;
                pkt.bytes.assign(
                    static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                    static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes
                );
                m_sv_packets.push_back(std::move(pkt));
            }

            st = m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
            if (st != NV_ENC_SUCCESS) {
                char buf[96];
                snprintf(buf, sizeof(buf), "frame %d: unlock bitstream: %s", frameIdx, NvencStatusName(st));
                PrintPhaseFail(25, "AV1 300-frame encode loop", buf);
                return false;
            }

        } else if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
            m_sv_framesSubmitted++;
            m_sv_needMoreInputCount++;

        } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "frame %d: %s", frameIdx, NvencStatusName(st));
            PrintPhaseFail(25, "AV1 300-frame encode loop", buf);
            return false;
        }

        if ((frameIdx + 1) % 60 == 0) {
            char detail[96];
            snprintf(detail, sizeof(detail),
                     "[frame %03d/300] submitted=%d packets=%d bytes=%llu",
                     frameIdx + 1, m_sv_framesSubmitted, m_sv_packetsLocked,
                     static_cast<unsigned long long>(m_sv_totalBytes));
            PrintDetail(detail);
        }
    }

    QueryPerformanceCounter(&tEnd);
    m_sv_elapsedSeconds = static_cast<double>(tEnd.QuadPart - tStart.QuadPart)
                        / static_cast<double>(freq.QuadPart);

    PrintPhasePass(25, "AV1 300-frame encode loop");
    return true;
}

bool BaselineProbe::Phase26_Av1SustainedDrainAndReport() {
    NV_ENC_PIC_PARAMS eosParams{};
    eosParams.version        = NV_ENC_PIC_PARAMS_VER;
    eosParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_funcs.nvEncEncodePicture(m_encoder, &eosParams);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(26, "AV1 sustained drain and report", NvencStatusName(st));
        return false;
    }

    // Conservative post-EOS drain: attempt up to needMoreInputCount times.
    // Stop early if a lock fails — do not treat that as a hard error.
    for (int i = 0; i < m_sv_needMoreInputCount; ++i) {
        NV_ENC_LOCK_BITSTREAM lockBS{};
        lockBS.version         = NV_ENC_LOCK_BITSTREAM_VER;
        lockBS.outputBitstream = m_bitstreamBuffer;
        lockBS.doNotWait       = 0;

        st = m_funcs.nvEncLockBitstream(m_encoder, &lockBS);
        if (st != NV_ENC_SUCCESS) {
            char note[96];
            snprintf(note, sizeof(note), "post-EOS drain stopped at attempt %d: %s",
                     i + 1, NvencStatusName(st));
            PrintDetail(note);
            break;
        }

        m_sv_totalBytes += lockBS.bitstreamSizeInBytes;
        m_sv_packetsLocked++;

        {
            Av1SustainedPacket pkt;
            pkt.timestamp = lockBS.outputTimeStamp;
            pkt.bytes.assign(
                static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes
            );
            m_sv_packets.push_back(std::move(pkt));
        }

        st = m_funcs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
        if (st != NV_ENC_SUCCESS) {
            PrintPhaseFail(26, "AV1 sustained drain and report", NvencStatusName(st));
            return false;
        }
    }

    if (m_sv_totalBytes == 0) {
        PrintPhaseFail(26, "AV1 sustained drain and report", "totalBytes == 0");
        return false;
    }

    PrintPhasePass(26, "AV1 sustained drain and report");

    double fps = (m_sv_elapsedSeconds > 0.0) ? (300.0 / m_sv_elapsedSeconds) : 0.0;
    char detail[96];
    snprintf(detail, sizeof(detail), "frames submitted  : %d", m_sv_framesSubmitted);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "packets locked    : %d", m_sv_packetsLocked);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "NEED_MORE_INPUT   : %d", m_sv_needMoreInputCount);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "total bytes       : %llu",
              static_cast<unsigned long long>(m_sv_totalBytes));
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "elapsed           : %.3f s", m_sv_elapsedSeconds);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "encode fps        : %.1f fps", fps);
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase27_WriteIvfFile() {
    if (m_sv_packets.empty()) {
        PrintPhaseFail(27, "write AV1 IVF file", "m_sv_packets is empty — no packets to write");
        return false;
    }
    if (m_sv_packets.size() > 300) {
        char buf[80];
        snprintf(buf, sizeof(buf), "unexpected packet count: %zu (expected <= 300)",
                 m_sv_packets.size());
        PrintPhaseFail(27, "write AV1 IVF file", buf);
        return false;
    }

    uint32_t ivfFrameCount = static_cast<uint32_t>(m_sv_packets.size());

    if (!CreateDirectoryW(L"probe_nvenc_output", nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            char buf[64];
            snprintf(buf, sizeof(buf), "CreateDirectoryW failed: error %lu",
                     static_cast<unsigned long>(err));
            PrintPhaseFail(27, "write AV1 IVF file", buf);
            return false;
        }
    }

    FILE* f = nullptr;
    errno_t e = fopen_s(&f, "probe_nvenc_output\\av1_300f.ivf", "wb");
    if (e != 0 || f == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "fopen_s failed: errno %d", static_cast<int>(e));
        PrintPhaseFail(27, "write AV1 IVF file", buf);
        return false;
    }

    auto writeU16 = [](FILE* fp, uint16_t v) -> bool {
        uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
        return fwrite(b, 1, 2, fp) == 2;
    };
    auto writeU32 = [](FILE* fp, uint32_t v) -> bool {
        uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) };
        return fwrite(b, 1, 4, fp) == 4;
    };
    auto writeU64 = [](FILE* fp, uint64_t v) -> bool {
        uint8_t b[8] = {
            uint8_t(v),     uint8_t(v>>8),  uint8_t(v>>16), uint8_t(v>>24),
            uint8_t(v>>32), uint8_t(v>>40), uint8_t(v>>48), uint8_t(v>>56)
        };
        return fwrite(b, 1, 8, fp) == 8;
    };

    bool ok = true;
    ok = ok && (fwrite("DKIF", 1, 4, f) == 4);
    ok = ok && writeU16(f, 0);
    ok = ok && writeU16(f, 32);
    ok = ok && (fwrite("AV01", 1, 4, f) == 4);
    ok = ok && writeU16(f, static_cast<uint16_t>(m_encodeWidth));
    ok = ok && writeU16(f, static_cast<uint16_t>(m_encodeHeight));
    ok = ok && writeU32(f, m_frameRateNum);
    ok = ok && writeU32(f, m_frameRateDen);
    ok = ok && writeU32(f, ivfFrameCount);
    ok = ok && writeU32(f, 0);

    if (!ok) {
        fclose(f);
        PrintPhaseFail(27, "write AV1 IVF file", "fwrite failed writing IVF file header");
        return false;
    }

    uint64_t bytesWritten = 32;
    for (const auto& pkt : m_sv_packets) {
        ok = writeU32(f, static_cast<uint32_t>(pkt.bytes.size()));
        ok = ok && writeU64(f, pkt.timestamp);
        ok = ok && (fwrite(pkt.bytes.data(), 1, pkt.bytes.size(), f) == pkt.bytes.size());
        if (!ok) {
            fclose(f);
            PrintPhaseFail(27, "write AV1 IVF file", "fwrite failed writing IVF frame data");
            return false;
        }
        bytesWritten += 12 + pkt.bytes.size();
    }

    fflush(f);
    fclose(f);

    PrintPhasePass(27, "write AV1 IVF file");
    PrintDetail("output: probe_nvenc_output\\av1_300f.ivf");
    char detail[96];
    snprintf(detail, sizeof(detail), "packets written   : %u", ivfFrameCount);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "ivf frame count   : %u", ivfFrameCount);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "bytes written     : %llu",
              static_cast<unsigned long long>(bytesWritten));
    PrintDetail(detail);
    return true;
}

bool BaselineProbe::Phase28_VerifyIvfFile() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(L"probe_nvenc_output\\av1_300f.ivf",
                              GetFileExInfoStandard, &data)) {
        PrintPhaseFail(28, "verify IVF file",
                       "GetFileAttributesExW failed — file not found or inaccessible");
        return false;
    }

    uint64_t fileSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

    if (fileSize <= 32) {
        char buf[80];
        snprintf(buf, sizeof(buf), "file size %llu <= IVF header size (32)",
                 static_cast<unsigned long long>(fileSize));
        PrintPhaseFail(28, "verify IVF file", buf);
        return false;
    }

    constexpr uint64_t kMaxExpected = 50ULL * 1024 * 1024;
    if (fileSize > kMaxExpected) {
        char buf[80];
        snprintf(buf, sizeof(buf), "file size %llu bytes unexpectedly large (> 50 MB)",
                 static_cast<unsigned long long>(fileSize));
        PrintPhaseFail(28, "verify IVF file", buf);
        return false;
    }

    PrintPhasePass(28, "verify IVF file");
    PrintDetail("file: probe_nvenc_output\\av1_300f.ivf");
    char detail[96];
    snprintf(detail, sizeof(detail), "size: %llu bytes",
             static_cast<unsigned long long>(fileSize));
    PrintDetail(detail);
    PrintDetail("expected ~13-14 KB for 300-frame deterministic AV1");
    return true;
}

bool BaselineProbe::RunAv1SustainedValidation() {
    fprintf(stdout, "[probe] === AV1 300-frame synthetic validation ===\n");
    fflush(stdout);

    struct Phase {
        const char* name;
        bool (BaselineProbe::*fn)();
    };
    const Phase phases[] = {
        {"open AV1 sustained session",     &BaselineProbe::Phase24_OpenAv1SustainedSession},
        {"AV1 300-frame encode loop",      &BaselineProbe::Phase25_Av1SustainedEncodeLoop},
        {"AV1 sustained drain and report", &BaselineProbe::Phase26_Av1SustainedDrainAndReport},
        {"write AV1 IVF file",             &BaselineProbe::Phase27_WriteIvfFile},
        {"verify IVF file",                &BaselineProbe::Phase28_VerifyIvfFile},
    };
    constexpr int kTotalPhases = sizeof(phases) / sizeof(phases[0]);

    for (int i = 0; i < kTotalPhases; ++i) {
        if (!(this->*(phases[i].fn))()) {
            fprintf(stdout, "STOP: AV1 sustained validation did not pass at %s.\n", phases[i].name);
            fflush(stdout);
            return false;
        }
    }
    return true;
}

int BaselineProbe::Run() {
    fprintf(stdout, "[probe] NVENC baseline bring-up probe (M2.4)\n");
    fprintf(stdout, "[probe] target: H.264/AV1 one-frame + AV1 300-frame synthetic encode + IVF file output\n");
    fflush(stdout);
    fprintf(stdout, "[probe] === H.264 one-frame validation ===\n");
    fflush(stdout);

    struct Phase {
        const char* name;
        bool (BaselineProbe::*fn)();
    };
    const Phase phases[] = {
        {"load nvEncodeAPI64.dll",              &BaselineProbe::Phase01_LoadDll},
        {"create NVENC API instance",           &BaselineProbe::Phase02_CreateApiInstance},
        {"create D3D11 device",                 &BaselineProbe::Phase03_CreateD3D11Device},
        {"open encode session",                 &BaselineProbe::Phase04_OpenEncodeSession},
        {"H.264 GUID support",                  &BaselineProbe::Phase05_QueryH264Support},
        {"NV12 input format support",           &BaselineProbe::Phase06_QueryNv12Support},
        {"fetch preset config",                 &BaselineProbe::Phase07_FetchPresetConfig},
        {"H.264 baseline init",                 &BaselineProbe::Phase08_InitializeEncoder},
        {"create NVENC input/output buffers",   &BaselineProbe::Phase09_CreateBuffers},
        {"fill one NV12 frame",                 &BaselineProbe::Phase10_FillFrame},
        {"submit one H.264 frame",              &BaselineProbe::Phase11_SubmitFrame},
        {"lock bitstream / verify bytes > 0",   &BaselineProbe::Phase12_LockBitstream},
        {"EOS flush and cleanup",               &BaselineProbe::Phase13_FlushAndCleanup},
    };
    constexpr int kTotalPhases = sizeof(phases) / sizeof(phases[0]);

    for (int i = 0; i < kTotalPhases; ++i) {
        if (!(this->*(phases[i].fn))()) {
            if (i + 1 < kTotalPhases) {
                fprintf(stdout, "STOP: later phases were not executed after failed %s.\n", phases[i].name);
            } else {
                fprintf(stdout, "STOP: baseline did not pass.\n");
            }
            fflush(stdout);
            return 1;
        }
    }

    if (!RunAv1Validation()) {
        return 1;
    }

    if (!RunAv1SustainedValidation()) {
        return 1;
    }

    fprintf(stdout, "[probe] encode probe PASS — H.264 one-frame, AV1 one-frame, AV1 300-frame, IVF file written and verified.\n");
    fflush(stdout);
    return 0;
}

} // namespace nvenc_probe
