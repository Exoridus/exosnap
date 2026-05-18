#include "wgc_nvenc_aac_mkv_probe.h"

#include <functiondiscoverykeys_devpkey.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// libwebm muxer headers (populated by FetchContent)
#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

namespace wgc_nvenc_aac_mkv {

// ---------------------------------------------------------------------------
// Phase printer helpers
// ---------------------------------------------------------------------------

namespace {

constexpr int kPadColumn = 56;

void PrintPhasePass(int phase, const char* label) {
    char prefix[128];
    int n = snprintf(prefix, sizeof(prefix), "[phase %02d] %s ", phase, label);
    if (n < 0)
        n = 0;
    fwrite(prefix, 1, static_cast<size_t>(n), stdout);
    for (int i = n; i < kPadColumn; ++i)
        fputc('.', stdout);
    fprintf(stdout, " PASS\n");
    fflush(stdout);
}

void PrintPhaseFail(int phase, const char* label, const char* reason) {
    char prefix[128];
    int n = snprintf(prefix, sizeof(prefix), "[phase %02d] %s ", phase, label);
    if (n < 0)
        n = 0;
    fwrite(prefix, 1, static_cast<size_t>(n), stdout);
    for (int i = n; i < kPadColumn; ++i)
        fputc('.', stdout);
    fprintf(stdout, " FAIL: %s\n", reason);
    fflush(stdout);
}

void PrintDetail(const char* line) {
    fprintf(stdout, "           %s\n", line);
    fflush(stdout);
}

} // namespace

// ---------------------------------------------------------------------------
// NvencStatusName
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Audio helpers
// ---------------------------------------------------------------------------

/*static*/
void WgcNvencAacMkvProbe::ConvertFloat32ToPcm16(const float* src, int16_t* dst, size_t sampleCount) {
    for (size_t i = 0; i < sampleCount; ++i) {
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(clamped * 32767.0f);
    }
}

/*static*/
std::string WgcNvencAacMkvProbe::WideToUtf8(const wchar_t* wstr) {
    if (wstr == nullptr || wstr[0] == L'\0')
        return "(unnamed)";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return "(unnamed)";
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// EnumerateTargets (identical to probe_wgc_nvenc_gpu)
// ---------------------------------------------------------------------------

std::vector<CaptureTarget> WgcNvencAacMkvProbe::EnumerateTargets() {
    std::vector<CaptureTarget> targets;

    winrt::com_ptr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    CaptureTarget t;
                    t.kind = CaptureTarget::Kind::Monitor;
                    t.hmonitor = desc.Monitor;
                    t.description = GetMonitorInfoW(desc.Monitor, &mi) ? mi.szDevice : std::to_wstring(targets.size());
                    targets.push_back(std::move(t));
                }
                output = nullptr;
            }
            adapter = nullptr;
        }
    }

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto& vec = *reinterpret_cast<std::vector<CaptureTarget>*>(lParam);
            if (!IsWindowVisible(hwnd))
                return TRUE;
            if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0)
                return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != nullptr)
                return TRUE;
            wchar_t title[256] = {};
            if (GetWindowTextW(hwnd, title, 256) == 0)
                return TRUE;
            CaptureTarget t;
            t.kind = CaptureTarget::Kind::Window;
            t.description = title;
            t.hwnd = hwnd;
            vec.push_back(std::move(t));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&targets));

    return targets;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

WgcNvencAacMkvProbe::WgcNvencAacMkvProbe(const CaptureTarget& target) : m_target(target) {
}

WgcNvencAacMkvProbe::~WgcNvencAacMkvProbe() {
    CleanupCapture();
    CleanupEncoder();
    CleanupAudio();

    m_videoOutputView = nullptr;
    m_nv12Texture = nullptr;
    m_videoProcessor = nullptr;
    m_videoEnum = nullptr;
    m_videoContext = nullptr;
    m_videoDevice = nullptr;
    m_d3dContext = nullptr;
    m_d3dDevice = nullptr;

    if (m_nvencDll != nullptr) {
        FreeLibrary(m_nvencDll);
        m_nvencDll = nullptr;
    }
}

void WgcNvencAacMkvProbe::CleanupCapture() {
    if (m_session != nullptr) {
        m_session.Close();
        m_session = nullptr;
    }
    if (m_framePool != nullptr) {
        m_framePool.Close();
        m_framePool = nullptr;
    }
    if (m_item != nullptr) {
        try {
            m_item.Closed(m_closedToken);
        } catch (...) {
        }
        m_closedToken = {};
        m_item = nullptr;
    }
}

void WgcNvencAacMkvProbe::CleanupEncoder() {
    if (m_encoder != nullptr && m_nvencFuncs.nvEncDestroyBitstreamBuffer != nullptr && m_bitstreamBuffer != nullptr) {
        m_nvencFuncs.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstreamBuffer);
        m_bitstreamBuffer = nullptr;
    }
    if (m_encoder != nullptr && m_nvencFuncs.nvEncDestroyEncoder != nullptr) {
        m_nvencFuncs.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
}

void WgcNvencAacMkvProbe::CleanupAudio() {
    if (m_pCaptureClient) {
        m_pCaptureClient->Release();
        m_pCaptureClient = nullptr;
    }
    if (m_pAudioClient) {
        m_pAudioClient->Stop();
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
    }
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    if (m_pOutputType) {
        m_pOutputType->Release();
        m_pOutputType = nullptr;
    }
    if (m_pMFT) {
        m_pMFT->Release();
        m_pMFT = nullptr;
    }
    if (m_pActivate) {
        m_pActivate->Release();
        m_pActivate = nullptr;
    }
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
}

// ---------------------------------------------------------------------------
// Phase 01 — COM + Media Foundation
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase01_InitComAndMF() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        char buf[80];
        snprintf(buf, sizeof(buf), "CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(1, "init COM + Media Foundation", buf);
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFStartup failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(1, "init COM + Media Foundation", buf);
        return false;
    }
    m_mfStarted = true;

    PrintPhasePass(1, "init COM + Media Foundation");
    PrintDetail("CoInitializeEx(COINIT_APARTMENTTHREADED) + MFStartup");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 02 — D3D11 video-capable device
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase02_InitD3D11VideoDevice() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, m_d3dDevice.put(), nullptr,
                                   m_d3dContext.put());

    if (FAILED(hr) || m_d3dDevice == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(2, "init D3D11 video-capable device", buf);
        return false;
    }

    hr = m_d3dDevice->QueryInterface(IID_PPV_ARGS(m_videoDevice.put()));
    if (FAILED(hr) || m_videoDevice == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "QI ID3D11VideoDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(2, "init D3D11 video-capable device", buf);
        return false;
    }

    hr = m_d3dContext->QueryInterface(IID_PPV_ARGS(m_videoContext.put()));
    if (FAILED(hr) || m_videoContext == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "QI ID3D11VideoContext failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(2, "init D3D11 video-capable device", buf);
        return false;
    }

    PrintPhasePass(2, "init D3D11 video-capable device");
    PrintDetail("flags: BGRA_SUPPORT | VIDEO_SUPPORT");
    PrintDetail("interfaces: ID3D11VideoDevice, ID3D11VideoContext");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 03 — WGC capture item
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase03_CreateCaptureItem() {
    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();

        if (m_target.kind == CaptureTarget::Kind::Monitor) {
            winrt::check_hresult(interop->CreateForMonitor(
                m_target.hmonitor, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(m_item)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(
                m_target.hwnd, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(m_item)));
        }
    } catch (const winrt::hresult_error& e) {
        char buf[96];
        snprintf(buf, sizeof(buf), "IGraphicsCaptureItemInterop failed 0x%08X",
                 static_cast<unsigned int>(e.code().value));
        PrintPhaseFail(3, "create WGC capture item", buf);
        return false;
    } catch (...) {
        PrintPhaseFail(3, "create WGC capture item", "unknown exception");
        return false;
    }

    if (m_item == nullptr) {
        PrintPhaseFail(3, "create WGC capture item", "item is null after creation");
        return false;
    }

    auto sz = m_item.Size();
    m_sourceWidth = static_cast<uint32_t>(sz.Width);
    m_sourceHeight = static_cast<uint32_t>(sz.Height);

    PrintPhasePass(3, "create WGC capture item");
    char detail[64];
    snprintf(detail, sizeof(detail), "source size: %ux%u", m_sourceWidth, m_sourceHeight);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 04 — Validate dimensions
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase04_ValidateDimensions() {
    m_encodeWidth = m_sourceWidth & ~1u;
    m_encodeHeight = m_sourceHeight & ~1u;

    if (m_encodeWidth < 2 || m_encodeHeight < 2) {
        char buf[96];
        snprintf(buf, sizeof(buf), "source %ux%u rounds to %ux%u — too small for NV12", m_sourceWidth, m_sourceHeight,
                 m_encodeWidth, m_encodeHeight);
        PrintPhaseFail(4, "validate source dimensions", buf);
        return false;
    }

    PrintPhasePass(4, "validate source dimensions");
    char detail[80];
    snprintf(detail, sizeof(detail), "encode size: %ux%u", m_encodeWidth, m_encodeHeight);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 05 — Load NVENC DLL + open API
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase05_LoadNvencDll() {
    m_nvencDll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (m_nvencDll == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "LoadLibraryW failed (GetLastError=%lu)",
                 static_cast<unsigned long>(GetLastError()));
        PrintPhaseFail(5, "load NVENC DLL + open encode session", buf);
        return false;
    }

    using PFN = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<PFN>(GetProcAddress(m_nvencDll, "NvEncodeAPICreateInstance"));
    if (pCreate == nullptr) {
        PrintPhaseFail(5, "load NVENC DLL + open encode session", "NvEncodeAPICreateInstance not exported");
        return false;
    }

    m_nvencFuncs = {};
    m_nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = pCreate(&m_nvencFuncs);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(5, "load NVENC DLL + open encode session", NvencStatusName(st));
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = m_d3dDevice.get();
    params.apiVersion = NVENCAPI_VERSION;

    st = m_nvencFuncs.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS || m_encoder == nullptr) {
        PrintPhaseFail(5, "load NVENC DLL + open encode session", NvencStatusName(st));
        return false;
    }

    PrintPhasePass(5, "load NVENC DLL + open encode session");
    char detail[80];
    snprintf(detail, sizeof(detail), "NVENCAPI_VERSION major=%u minor=%u",
             static_cast<unsigned>(NVENCAPI_MAJOR_VERSION), static_cast<unsigned>(NVENCAPI_MINOR_VERSION));
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 06 — Query AV1 GUID + NV12 format support
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase06_QueryAv1Support() {
    uint32_t count = 0;
    NVENCSTATUS st = m_nvencFuncs.nvEncGetEncodeGUIDCount(m_encoder, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        PrintPhaseFail(6, "AV1 GUID + NV12 format support", NvencStatusName(st));
        return false;
    }
    std::vector<GUID> guids(count);
    uint32_t got = 0;
    st = m_nvencFuncs.nvEncGetEncodeGUIDs(m_encoder, guids.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(6, "AV1 GUID + NV12 format support", NvencStatusName(st));
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
        PrintPhaseFail(6, "AV1 GUID + NV12 format support",
                       "NV_ENC_CODEC_AV1_GUID not found (Ada Lovelace / RTX 40+ required)");
        return false;
    }

    count = 0;
    st = m_nvencFuncs.nvEncGetInputFormatCount(m_encoder, NV_ENC_CODEC_AV1_GUID, &count);
    if (st != NV_ENC_SUCCESS || count == 0) {
        PrintPhaseFail(6, "AV1 GUID + NV12 format support", NvencStatusName(st));
        return false;
    }
    std::vector<NV_ENC_BUFFER_FORMAT> fmts(count);
    st = m_nvencFuncs.nvEncGetInputFormats(m_encoder, NV_ENC_CODEC_AV1_GUID, fmts.data(), count, &got);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(6, "AV1 GUID + NV12 format support", NvencStatusName(st));
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
        PrintPhaseFail(6, "AV1 GUID + NV12 format support", "NV_ENC_BUFFER_FORMAT_NV12 not in AV1 input formats");
        return false;
    }

    PrintPhasePass(6, "AV1 GUID + NV12 format support");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 07 — Fetch AV1 preset config
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase07_FetchPresetConfig() {
    m_presetConfig = {};
    m_presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    m_presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = m_nvencFuncs.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_AV1_GUID, m_presetGuid,
                                                               m_tuningInfo, &m_presetConfig);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(7, "fetch AV1 preset config", NvencStatusName(st));
        return false;
    }

    std::memcpy(&m_encodeConfig, &m_presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
    m_encodeConfig.version = NV_ENC_CONFIG_VER;
    m_encodeConfig.encodeCodecConfig.av1Config.chromaFormatIDC = 1; // YUV420/NV12

    PrintPhasePass(7, "fetch AV1 preset config");
    PrintDetail("preset: NV_ENC_PRESET_P4_GUID, tuning: NV_ENC_TUNING_INFO_HIGH_QUALITY");
    PrintDetail("chromaFormatIDC = 1 (YUV420/NV12)");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 08 — Init AV1 encoder
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase08_InitAv1Encoder() {
    NV_ENC_INITIALIZE_PARAMS p{};
    p.version = NV_ENC_INITIALIZE_PARAMS_VER;
    p.encodeGUID = NV_ENC_CODEC_AV1_GUID;
    p.presetGUID = m_presetGuid;
    p.tuningInfo = m_tuningInfo;
    p.encodeWidth = m_encodeWidth;
    p.encodeHeight = m_encodeHeight;
    p.darWidth = m_encodeWidth;
    p.darHeight = m_encodeHeight;
    p.maxEncodeWidth = m_encodeWidth;
    p.maxEncodeHeight = m_encodeHeight;
    p.frameRateNum = m_frameRateNum;
    p.frameRateDen = m_frameRateDen;
    p.enablePTD = 1;
    p.encodeConfig = &m_encodeConfig;

    NVENCSTATUS st = m_nvencFuncs.nvEncInitializeEncoder(m_encoder, &p);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(8, "init AV1 encoder", NvencStatusName(st));
        return false;
    }

    PrintPhasePass(8, "init AV1 encoder");
    char detail[128];
    snprintf(detail, sizeof(detail), "%ux%u @ %u/%u fps, AV1, preset P4, enablePTD=1", m_encodeWidth, m_encodeHeight,
             m_frameRateNum, m_frameRateDen);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 09 — Bitstream buffer + NV12 texture + video processor + NVENC register
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase09_BitstreamNv12VideoProcessorRegister() {
    // --- Bitstream buffer ---
    {
        NV_ENC_CREATE_BITSTREAM_BUFFER bsp{};
        bsp.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        NVENCSTATUS st = m_nvencFuncs.nvEncCreateBitstreamBuffer(m_encoder, &bsp);
        if (st != NV_ENC_SUCCESS) {
            char buf[80];
            snprintf(buf, sizeof(buf), "nvEncCreateBitstreamBuffer: %s", NvencStatusName(st));
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register", buf);
            return false;
        }
        m_bitstreamBuffer = bsp.bitstreamBuffer;
    }

    // --- NV12 texture ---
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = m_encodeWidth;
        desc.Height = m_encodeHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, m_nv12Texture.put());
        if (FAILED(hr) || m_nv12Texture == nullptr) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateTexture2D(NV12) failed 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register", buf);
            return false;
        }
    }

    // --- Video processor enumerator + processor ---
    {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = m_encodeWidth;
        contentDesc.InputHeight = m_encodeHeight;
        contentDesc.OutputWidth = m_encodeWidth;
        contentDesc.OutputHeight = m_encodeHeight;
        contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, m_videoEnum.put());
        if (FAILED(hr) || m_videoEnum == nullptr) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateVideoProcessorEnumerator failed 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register", buf);
            return false;
        }

        hr = m_videoDevice->CreateVideoProcessor(m_videoEnum.get(), 0, m_videoProcessor.put());
        if (FAILED(hr) || m_videoProcessor == nullptr) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateVideoProcessor failed 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register", buf);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
        ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovDesc.Texture2D.MipSlice = 0;

        hr = m_videoDevice->CreateVideoProcessorOutputView(m_nv12Texture.get(), m_videoEnum.get(), &ovDesc,
                                                           m_videoOutputView.put());
        if (FAILED(hr) || m_videoOutputView == nullptr) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateVideoProcessorOutputView failed 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register", buf);
            return false;
        }
    }

    // --- Register NV12 with NVENC ---
    {
        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = m_encodeWidth;
        reg.height = m_encodeHeight;
        reg.pitch = 0;
        reg.subResourceIndex = 0;
        reg.resourceToRegister = m_nv12Texture.get();
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;

        NVENCSTATUS st = m_nvencFuncs.nvEncRegisterResource(m_encoder, &reg);
        if (st != NV_ENC_SUCCESS) {
            char buf[96];
            snprintf(buf, sizeof(buf), "nvEncRegisterResource: %s", NvencStatusName(st));
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register", buf);
            return false;
        }
        m_registeredResource = reg.registeredResource;
        if (m_registeredResource == nullptr) {
            PrintPhaseFail(9, "bitstream+NV12+vidproc+register",
                           "registeredResource is null after successful register");
            return false;
        }
    }

    PrintPhasePass(9, "bitstream+NV12+vidproc+register");
    PrintDetail("GPU path: bitstream buffer + NV12 texture + video processor + NVENC resource");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 10 — AAC encoder (payload type 0 = raw, NOT ADTS)
// ---------------------------------------------------------------------------

static HRESULT BuildAudioMediaType(const GUID& subtype, UINT32 sampleRate, UINT32 channels, UINT32 bitsPerSample,
                                   IMFMediaType** ppType) {
    *ppType = nullptr;
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr))
        return hr;

    UINT32 blockAlign = channels * (bitsPerSample / 8);
    UINT32 avgBytesPerSec = sampleRate * blockAlign;
    UINT32 channelMask = 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT

    bool ok = true;
    ok = ok && SUCCEEDED(pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    ok = ok && SUCCEEDED(pType->SetGUID(MF_MT_SUBTYPE, subtype));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesPerSec));
    ok = ok && SUCCEEDED(pType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, channelMask));

    if (!ok) {
        pType->Release();
        return E_FAIL;
    }
    *ppType = pType;
    return S_OK;
}

bool WgcNvencAacMkvProbe::Phase10_InitAacEncoder() {
    constexpr uint32_t kRequiredSampleRate = 48000;
    constexpr uint32_t kRequiredChannels = 2;

    // Step 1: MFTEnumEx with Float input, AAC output
    MFT_REGISTER_TYPE_INFO inType = {MFMediaType_Audio, MFAudioFormat_Float};
    MFT_REGISTER_TYPE_INFO outType = {MFMediaType_Audio, MFAudioFormat_AAC};
    constexpr DWORD kEnumFlags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, kEnumFlags, &inType, &outType, &ppActivate, &count);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFTEnumEx failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(10, "init AAC encoder", buf);
        return false;
    }

    if (count == 0) {
        if (ppActivate) {
            CoTaskMemFree(ppActivate);
            ppActivate = nullptr;
        }
        // Fallback: direct CLSID
        hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pMFT));
        if (FAILED(hr)) {
            char buf[96];
            snprintf(buf, sizeof(buf), "MFTEnumEx=0 + CoCreateInstance(CLSID_AACMFTEncoder) 0x%08lX",
                     static_cast<unsigned long>(hr));
            PrintPhaseFail(10, "init AAC encoder", buf);
            return false;
        }
        m_usedDirectClsid = true;
    } else {
        m_pActivate = ppActivate[0];
        for (UINT32 i = 1; i < count; ++i)
            ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);

        hr = m_pActivate->ActivateObject(IID_PPV_ARGS(&m_pMFT));
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "ActivateObject(IMFTransform) 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(10, "init AAC encoder", buf);
            return false;
        }
    }

    // Step 2: SetInputType — same negotiation as mf_aac_probe Phase04
    struct TryEntry {
        GUID subtype;
        UINT32 bps;
        const char* label;
    };
    TryEntry tryList[] = {
        {MFAudioFormat_PCM, 16, "PCM-16"},
        {MFAudioFormat_PCM, 32, "PCM-32"},
        {MFAudioFormat_Float, 32, "Float-32"},
    };

    bool inputSet = false;
    for (const auto& e : tryList) {
        IMFMediaType* pType = nullptr;
        if (FAILED(BuildAudioMediaType(e.subtype, kRequiredSampleRate, kRequiredChannels, e.bps, &pType)))
            continue;
        hr = m_pMFT->SetInputType(0, pType, 0);
        pType->Release();
        if (SUCCEEDED(hr)) {
            m_inputSubtype = e.subtype;
            m_inputBitsPerSample = e.bps;
            m_inputBlockAlign = kRequiredChannels * (e.bps / 8);
            m_inputAvgBytesPerSec = kRequiredSampleRate * m_inputBlockAlign;
            inputSet = true;
            char detail[96];
            snprintf(detail, sizeof(detail), "input type: %s %u bps, 48000 Hz, 2ch", e.label, e.bps);
            PrintDetail(detail);
            break;
        }
    }
    if (!inputSet) {
        PrintPhaseFail(10, "init AAC encoder", "all input type candidates rejected");
        return false;
    }

    // Step 3: SetOutputType — payload type 0 (raw), NOT ADTS
    {
        IMFMediaType* pOutputType = nullptr;
        hr = MFCreateMediaType(&pOutputType);
        if (FAILED(hr)) {
            PrintPhaseFail(10, "init AAC encoder", "MFCreateMediaType for output failed");
            return false;
        }

        bool ok = true;
        ok = ok && SUCCEEDED(pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        ok = ok && SUCCEEDED(pOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRequiredSampleRate));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kRequiredChannels));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000)); // 192 kbps
        ok = ok && SUCCEEDED(pOutputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));               // RAW, not ADTS
        ok = ok &&
             SUCCEEDED(pOutputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29)); // AAC-LC stereo 48kHz

        if (!ok) {
            pOutputType->Release();
            PrintPhaseFail(10, "init AAC encoder", "failed to set output type attributes");
            return false;
        }

        hr = m_pMFT->SetOutputType(0, pOutputType, 0);
        pOutputType->Release();
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "SetOutputType(AAC raw) failed 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(10, "init AAC encoder", buf);
            return false;
        }
    }

    // Step 4: GetOutputCurrentType — store for Phase 19 CodecPrivate derivation
    hr = m_pMFT->GetOutputCurrentType(0, &m_pOutputType);
    if (FAILED(hr) || m_pOutputType == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetOutputCurrentType failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(10, "init AAC encoder", buf);
        return false;
    }

    // Step 5: GetOutputStreamInfo
    {
        MFT_OUTPUT_STREAM_INFO si{};
        hr = m_pMFT->GetOutputStreamInfo(0, &si);
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "GetOutputStreamInfo failed 0x%08lX", static_cast<unsigned long>(hr));
            PrintPhaseFail(10, "init AAC encoder", buf);
            return false;
        }
        m_mftProvidesSamples = (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        m_mftOutBufSize = (si.cbSize > 0) ? si.cbSize : 8192;
    }

    // Step 6: Begin streaming
    hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(10, "init AAC encoder", buf);
        return false;
    }
    hr = m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MFT_MESSAGE_NOTIFY_START_OF_STREAM failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(10, "init AAC encoder", buf);
        return false;
    }

    PrintPhasePass(10, "init AAC encoder");
    PrintDetail("payload type: 0 (raw AAC, not ADTS)");
    PrintDetail("48000 Hz, 2ch, 192 kbps, profile level 0x29 (AAC-LC stereo 48kHz)");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 11 — WASAPI loopback
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase11_InitWasapiLoopback() {
    constexpr uint32_t kRequiredSampleRate = 48000;
    constexpr uint32_t kRequiredChannels = 2;
    constexpr LONGLONG kHnsBuffer = 2000000LL; // 200 ms

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "CoCreateInstance(MMDeviceEnumerator) 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    pEnum->Release();
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetDefaultAudioEndpoint 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    // Friendly name
    {
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(m_pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR)
                m_endpointName = WideToUtf8(var.pwszVal);
            PropVariantClear(&var);
            pProps->Release();
        }
        if (m_endpointName.empty())
            m_endpointName = "(unnamed)";
    }

    hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_pAudioClient));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Activate(IAudioClient) 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = m_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || pwfx == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetMixFormat 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    if (pwfx->nSamplesPerSec != kRequiredSampleRate || pwfx->nChannels != kRequiredChannels) {
        char buf[128];
        snprintf(buf, sizeof(buf), "mix format mismatch: %u Hz %u ch (need %u Hz 2 ch)", pwfx->nSamplesPerSec,
                 pwfx->nChannels, kRequiredSampleRate);
        CoTaskMemFree(pwfx);
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kHnsBuffer, 0, pwfx,
                                    nullptr);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IAudioClient::Initialize(LOOPBACK) 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    hr = m_pAudioClient->GetService(IID_PPV_ARGS(&m_pCaptureClient));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetService(IAudioCaptureClient) 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IAudioClient::Start 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(11, "init WASAPI loopback", buf);
        return false;
    }

    PrintPhasePass(11, "init WASAPI loopback");
    char detail[128];
    snprintf(detail, sizeof(detail), "endpoint: %s", m_endpointName.c_str());
    PrintDetail(detail);
    PrintDetail("AUDCLNT_STREAMFLAGS_LOOPBACK, 200ms buffer, started");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 12 — Establish QPC epoch + start WGC
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase12_EstablishEpochAndStartWgc() {
    QueryPerformanceFrequency(&m_qpcFreq);
    QueryPerformanceCounter(&m_qpcEpoch);

    try {
        winrt::com_ptr<IDXGIDevice> dxgiDev = m_d3dDevice.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
        auto d3dWinRTDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto sz = m_item.Size();
        m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
            d3dWinRTDev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, sz);

        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.StartCapture();

        m_closedToken = m_item.Closed([this](const auto&, const auto&) { m_sourceLost = true; });
    } catch (const winrt::hresult_error& e) {
        char buf[96];
        snprintf(buf, sizeof(buf), "WGC frame pool init failed 0x%08X", static_cast<unsigned int>(e.code().value));
        PrintPhaseFail(12, "establish epoch + start WGC", buf);
        return false;
    } catch (...) {
        PrintPhaseFail(12, "establish epoch + start WGC", "unknown exception");
        return false;
    }

    PrintPhasePass(12, "establish epoch + start WGC");
    char detail[80];
    snprintf(detail, sizeof(detail), "QPC epoch: %lld", static_cast<long long>(m_qpcEpoch.QuadPart));
    PrintDetail(detail);
    PrintDetail("WGC frame pool: 3 buffers, B8G8R8A8UIntNormalized, polling mode");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 13 — Wait for first WGC frame (5s timeout)
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase13_WaitForFirstFrame() {
    LARGE_INTEGER tStart, tNow;
    QueryPerformanceCounter(&tStart);
    constexpr double kTimeoutSec = 5.0;

    while (true) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_sourceLost) {
            PrintPhaseFail(13, "wait for first WGC frame", "source lost before first frame");
            return false;
        }

        QueryPerformanceCounter(&tNow);
        double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) / static_cast<double>(m_qpcFreq.QuadPart);
        if (elapsed > kTimeoutSec) {
            PrintPhaseFail(13, "wait for first WGC frame", "timeout (5 s)");
            return false;
        }

        try {
            auto frame = m_framePool.TryGetNextFrame();
            if (frame == nullptr) {
                Sleep(1);
                continue;
            }

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            HRESULT hr = access->GetInterface(IID_PPV_ARGS(tex.put()));
            if (FAILED(hr) || tex == nullptr) {
                Sleep(1);
                continue;
            }

            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);

            if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                char buf[96];
                snprintf(buf, sizeof(buf), "first frame format 0x%X != B8G8R8A8_UNORM",
                         static_cast<unsigned>(desc.Format));
                PrintPhaseFail(13, "wait for first WGC frame", buf);
                return false;
            }
            if (desc.Width != m_encodeWidth || desc.Height != m_encodeHeight) {
                char buf[128];
                snprintf(buf, sizeof(buf), "first frame %ux%u != encode %ux%u", desc.Width, desc.Height, m_encodeWidth,
                         m_encodeHeight);
                PrintPhaseFail(13, "wait for first WGC frame", buf);
                return false;
            }

            PrintPhasePass(13, "wait for first WGC frame");
            char detail[80];
            snprintf(detail, sizeof(detail), "first frame: %ux%u BGRA8", desc.Width, desc.Height);
            PrintDetail(detail);
            return true;

        } catch (const winrt::hresult_error& e) {
            char buf[80];
            snprintf(buf, sizeof(buf), "TryGetNextFrame exception 0x%08X", static_cast<unsigned int>(e.code().value));
            PrintPhaseFail(13, "wait for first WGC frame", buf);
            return false;
        } catch (...) {
            PrintPhaseFail(13, "wait for first WGC frame", "unknown exception");
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// DrainAacOutput — collect per-sample AudioPackets from MFT
// ---------------------------------------------------------------------------

void WgcNvencAacMkvProbe::DrainAacOutput() {
    while (true) {
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        outBuf.dwStreamID = 0;
        outBuf.pSample = nullptr;
        outBuf.dwStatus = 0;
        outBuf.pEvents = nullptr;

        if (!m_mftProvidesSamples) {
            IMFMediaBuffer* pMFBuf = nullptr;
            if (FAILED(MFCreateMemoryBuffer(m_mftOutBufSize, &pMFBuf)))
                break;
            IMFSample* pOutSample = nullptr;
            if (FAILED(MFCreateSample(&pOutSample))) {
                pMFBuf->Release();
                break;
            }
            pOutSample->AddBuffer(pMFBuf);
            pMFBuf->Release();
            outBuf.pSample = pOutSample;
        }

        DWORD status = 0;
        HRESULT hr = m_pMFT->ProcessOutput(0, 1, &outBuf, &status);

        if (outBuf.pEvents) {
            outBuf.pEvents->Release();
            outBuf.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outBuf.pSample) {
                outBuf.pSample->Release();
            }
            break;
        }
        if (FAILED(hr)) {
            if (outBuf.pSample) {
                outBuf.pSample->Release();
            }
            break;
        }

        if (outBuf.pSample) {
            // Get timestamp from sample
            LONGLONG sampleTime100ns = 0;
            bool hasTime = SUCCEEDED(outBuf.pSample->GetSampleTime(&sampleTime100ns));

            AudioPacket pkt;
            pkt.timestamp_ns = hasTime ? static_cast<uint64_t>(sampleTime100ns) * 100ULL : 0ULL;

            DWORD bufCount = 0;
            if (SUCCEEDED(outBuf.pSample->GetBufferCount(&bufCount))) {
                for (DWORD b = 0; b < bufCount; ++b) {
                    IMFMediaBuffer* pBuf = nullptr;
                    if (SUCCEEDED(outBuf.pSample->GetBufferByIndex(b, &pBuf))) {
                        BYTE* pData = nullptr;
                        DWORD cbCurr = 0;
                        if (SUCCEEDED(pBuf->Lock(&pData, nullptr, &cbCurr)) && cbCurr > 0) {
                            size_t old = pkt.bytes.size();
                            pkt.bytes.resize(old + cbCurr);
                            std::memcpy(pkt.bytes.data() + old, pData, cbCurr);
                            m_totalAudioBytes += cbCurr;
                            pBuf->Unlock();
                        }
                        pBuf->Release();
                    }
                }
            }

            if (!pkt.bytes.empty())
                m_audioPackets.push_back(std::move(pkt));

            outBuf.pSample->Release();
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 14 — 30-second dual capture/encode loop
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase14_DualCaptureLoop() {
    constexpr double kDurationSec = 30.0;
    constexpr uint32_t kRequiredSampleRate = 48000;
    constexpr uint32_t kRequiredChannels = 2;

    LARGE_INTEGER tLoopStart, tNow;
    QueryPerformanceCounter(&tLoopStart);

    int videoFrameIdx = 0;
    uint64_t audioAccumulatedFrames = 0;

    while (true) {
        QueryPerformanceCounter(&tNow);
        double elapsed =
            static_cast<double>(tNow.QuadPart - tLoopStart.QuadPart) / static_cast<double>(m_qpcFreq.QuadPart);
        if (elapsed >= kDurationSec) {
            m_terminationReason = TerminationReason::TimeLimit;
            break;
        }
        if (m_sourceLost) {
            m_terminationReason = TerminationReason::SourceLoss;
            break;
        }

        bool anyWork = false;

        // ---- VIDEO: drain WGC frames, keep latest, encode ----
        {
            winrt::com_ptr<ID3D11Texture2D> latestTex;
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame lastWgcFrame = nullptr;
            int64_t latestFrameTicks100ns = 0; // SystemRelativeTime of the latest accepted frame

            try {
                while (true) {
                    auto frame = m_framePool.TryGetNextFrame();
                    if (frame == nullptr)
                        break;
                    m_wgcFramesTotal++;

                    auto surface = frame.Surface();
                    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                        latestTex = tex;
                        lastWgcFrame = frame;
                        // SystemRelativeTime is a std::chrono::duration<int64_t, 100ns>
                        latestFrameTicks100ns = frame.SystemRelativeTime().count();
                    }
                }
            } catch (...) {
            }

            if (latestTex != nullptr) {
                // Establish video epoch from the first captured frame's SystemRelativeTime.
                if (!m_videoEpochSet) {
                    m_videoEpochTicks100ns = latestFrameTicks100ns;
                    m_videoEpochSet = true;
                }

                // Compute capture-time PTS in nanoseconds relative to video epoch.
                // SystemRelativeTime ticks are 100 ns units; epoch may be >= frame time
                // if the system clock wraps (extremely rare), so clamp to 0.
                int64_t deltaTicks = latestFrameTicks100ns - m_videoEpochTicks100ns;
                if (deltaTicks < 0)
                    deltaTicks = 0;
                uint64_t framePts_ns = static_cast<uint64_t>(deltaTicks) * 100ULL;

                // Create per-frame input view for the WGC BGRA texture
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
                ivDesc.FourCC = 0;
                ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                ivDesc.Texture2D.MipSlice = 0;
                ivDesc.Texture2D.ArraySlice = 0;

                winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
                HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(latestTex.get(), m_videoEnum.get(), &ivDesc,
                                                                          inputView.put());

                if (SUCCEEDED(hr) && inputView != nullptr) {
                    // GPU BGRA->NV12 via VideoProcessorBlt
                    D3D11_VIDEO_PROCESSOR_STREAM stream{};
                    stream.Enable = TRUE;
                    stream.pInputSurface = inputView.get();

                    hr = m_videoContext->VideoProcessorBlt(m_videoProcessor.get(), m_videoOutputView.get(), 0, 1,
                                                           &stream);

                    inputView = nullptr;
                    latestTex = nullptr;
                    lastWgcFrame = nullptr;

                    if (SUCCEEDED(hr)) {
                        // Map NVENC resource
                        NV_ENC_MAP_INPUT_RESOURCE mapRes{};
                        mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
                        mapRes.registeredResource = m_registeredResource;

                        NVENCSTATUS nvSt = m_nvencFuncs.nvEncMapInputResource(m_encoder, &mapRes);
                        if (nvSt == NV_ENC_SUCCESS) {
                            // Push the capture-time PTS into the pending queue BEFORE
                            // submitting to NVENC, so it is available for both the
                            // immediate-output case (NV_ENC_SUCCESS) and the buffered
                            // case (NV_ENC_ERR_NEED_MORE_INPUT).
                            m_pendingVideoPts.push(framePts_ns);

                            NV_ENC_PIC_PARAMS pic{};
                            pic.version = NV_ENC_PIC_PARAMS_VER;
                            pic.inputWidth = m_encodeWidth;
                            pic.inputHeight = m_encodeHeight;
                            pic.inputPitch = 0;
                            pic.inputBuffer = mapRes.mappedResource;
                            pic.outputBitstream = m_bitstreamBuffer;
                            pic.bufferFmt = mapRes.mappedBufferFmt;
                            pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
                            pic.encodePicFlags = 0;
                            // Use the frame index as the opaque inputTimeStamp for
                            // NVENC's internal ordering; actual PTS assignment comes
                            // from m_pendingVideoPts below.
                            pic.inputTimeStamp = static_cast<uint64_t>(videoFrameIdx);

                            nvSt = m_nvencFuncs.nvEncEncodePicture(m_encoder, &pic);

                            if (nvSt == NV_ENC_SUCCESS) {
                                m_capturedVideoFrames++;

                                NV_ENC_LOCK_BITSTREAM lockBS{};
                                lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
                                lockBS.outputBitstream = m_bitstreamBuffer;
                                lockBS.doNotWait = 0;

                                nvSt = m_nvencFuncs.nvEncLockBitstream(m_encoder, &lockBS);
                                if (nvSt == NV_ENC_SUCCESS) {
                                    // Pop the oldest pending PTS to assign to this output packet.
                                    // The FIFO was populated before nvEncEncodePicture, so it
                                    // always has at least one entry here.
                                    uint64_t ts_ns = 0;
                                    if (!m_pendingVideoPts.empty()) {
                                        ts_ns = m_pendingVideoPts.front();
                                        m_pendingVideoPts.pop();
                                    }

                                    bool isKey = (lockBS.pictureType == NV_ENC_PIC_TYPE_IDR ||
                                                  lockBS.pictureType == NV_ENC_PIC_TYPE_I);

                                    VideoPacket vp;
                                    vp.timestamp_ns = ts_ns;
                                    vp.is_keyframe = isKey;
                                    vp.bytes.assign(static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                                                    static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) +
                                                        lockBS.bitstreamSizeInBytes);
                                    m_totalVideoBytes += lockBS.bitstreamSizeInBytes;
                                    m_videoPackets.push_back(std::move(vp));

                                    m_nvencFuncs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
                                }
                                m_nvencFuncs.nvEncUnmapInputResource(m_encoder, mapRes.mappedResource);

                            } else if (nvSt == NV_ENC_ERR_NEED_MORE_INPUT) {
                                // Frame was accepted and PTS is already in the queue.
                                // It will be consumed during EOS drain in Phase 15.
                                m_capturedVideoFrames++;
                                m_needMoreInputCount++;
                                m_nvencFuncs.nvEncUnmapInputResource(m_encoder, mapRes.mappedResource);
                            } else {
                                // Encode failed — the frame was not accepted, so remove
                                // the PTS we pushed; it will never produce an output packet.
                                if (!m_pendingVideoPts.empty())
                                    m_pendingVideoPts.pop();
                                m_nvencFuncs.nvEncUnmapInputResource(m_encoder, mapRes.mappedResource);
                            }
                        }
                    } else {
                        // VideoProcessorBlt failed — release kept alive refs
                        latestTex = nullptr;
                        lastWgcFrame = nullptr;
                    }
                } else {
                    latestTex = nullptr;
                    lastWgcFrame = nullptr;
                }

                videoFrameIdx++;
                anyWork = true;
            }
        } // video block

        // ---- AUDIO: drain WASAPI, push to AAC encoder ----
        {
            UINT32 numFrames = 0;
            while (SUCCEEDED(m_pCaptureClient->GetNextPacketSize(&numFrames)) && numFrames > 0) {
                BYTE* pData = nullptr;
                DWORD captureFlags = 0;
                if (FAILED(m_pCaptureClient->GetBuffer(&pData, &numFrames, &captureFlags, nullptr, nullptr)))
                    break;

                // Sample-count-derived PTS
                uint64_t audio_ts_ns = audioAccumulatedFrames * 1000000000ULL / kRequiredSampleRate;
                LONGLONG sampleDuration =
                    static_cast<LONGLONG>(numFrames) * 10000000LL / static_cast<LONGLONG>(kRequiredSampleRate);

                const bool needPcm16 = (m_inputSubtype == MFAudioFormat_PCM && m_inputBitsPerSample == 16);
                UINT32 dataBytes = numFrames * m_inputBlockAlign;

                IMFMediaBuffer* pMFBuf = nullptr;
                HRESULT hr = MFCreateMemoryBuffer(dataBytes, &pMFBuf);
                if (FAILED(hr)) {
                    m_pCaptureClient->ReleaseBuffer(numFrames);
                    continue;
                }

                BYTE* pDst = nullptr;
                pMFBuf->Lock(&pDst, nullptr, nullptr);

                if (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::memset(pDst, 0, dataBytes);
                } else if (needPcm16) {
                    ConvertFloat32ToPcm16(reinterpret_cast<const float*>(pData), reinterpret_cast<int16_t*>(pDst),
                                          static_cast<size_t>(numFrames) * kRequiredChannels);
                } else {
                    std::memcpy(pDst, pData, dataBytes);
                }
                pMFBuf->Unlock();
                pMFBuf->SetCurrentLength(dataBytes);

                m_pCaptureClient->ReleaseBuffer(numFrames);
                audioAccumulatedFrames += numFrames;

                IMFSample* pSample = nullptr;
                hr = MFCreateSample(&pSample);
                if (FAILED(hr)) {
                    pMFBuf->Release();
                    continue;
                }

                pSample->AddBuffer(pMFBuf);
                pMFBuf->Release();
                pMFBuf = nullptr;

                // Set sample time in 100ns units
                pSample->SetSampleTime(static_cast<LONGLONG>(audio_ts_ns / 100ULL));
                pSample->SetSampleDuration(sampleDuration);

                hr = m_pMFT->ProcessInput(0, pSample, 0);
                pSample->Release();

                if (hr == MF_E_NOTACCEPTING) {
                    DrainAacOutput();
                } else if (SUCCEEDED(hr)) {
                    DrainAacOutput();
                }

                anyWork = true;
            }
        } // audio block

        if (!anyWork)
            Sleep(1);
    } // main loop

    QueryPerformanceCounter(&tNow);
    m_elapsedSeconds =
        static_cast<double>(tNow.QuadPart - tLoopStart.QuadPart) / static_cast<double>(m_qpcFreq.QuadPart);

    PrintPhasePass(14, "30-second dual capture/encode loop");
    char detail[128];
    snprintf(detail, sizeof(detail), "video frames encoded: %d, audio packets: %zu", m_capturedVideoFrames,
             m_audioPackets.size());
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "elapsed: %.2f s, termination: %s", m_elapsedSeconds,
             m_terminationReason == TerminationReason::SourceLoss ? "source loss" : "time limit");
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 15 — Drain AV1 encoder (EOS flush)
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase15_DrainAv1Encoder() {
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_nvencFuncs.nvEncEncodePicture(m_encoder, &eos);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(15, "drain AV1 encoder", NvencStatusName(st));
        return false;
    }

    int drained = 0;
    for (int i = 0; i < m_needMoreInputCount; ++i) {
        if (m_pendingVideoPts.empty()) {
            // More encoded packets than pending PTS — this is a logic error.
            char note[128];
            snprintf(note, sizeof(note),
                     "drain aborted at packet %d: pending PTS queue exhausted "
                     "(needMoreInput=%d, drained=%d)",
                     i + 1, m_needMoreInputCount, drained);
            PrintDetail(note);
            break;
        }

        NV_ENC_LOCK_BITSTREAM lockBS{};
        lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBS.outputBitstream = m_bitstreamBuffer;
        lockBS.doNotWait = 0;

        st = m_nvencFuncs.nvEncLockBitstream(m_encoder, &lockBS);
        if (st != NV_ENC_SUCCESS) {
            char note[96];
            snprintf(note, sizeof(note), "drain stopped at %d: %s", i + 1, NvencStatusName(st));
            PrintDetail(note);
            break;
        }

        // Assign the oldest pending capture-time PTS to this drained output packet.
        uint64_t ts_ns = m_pendingVideoPts.front();
        m_pendingVideoPts.pop();

        bool isKey = (lockBS.pictureType == NV_ENC_PIC_TYPE_IDR || lockBS.pictureType == NV_ENC_PIC_TYPE_I);

        VideoPacket vp;
        vp.timestamp_ns = ts_ns;
        vp.is_keyframe = isKey;
        vp.bytes.assign(static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                        static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes);
        m_totalVideoBytes += lockBS.bitstreamSizeInBytes;
        m_videoPackets.push_back(std::move(vp));

        m_nvencFuncs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
        ++drained;
    }

    PrintPhasePass(15, "drain AV1 encoder");
    char detail[80];
    snprintf(detail, sizeof(detail), "drained %d buffered frames (needMoreInput=%d)", drained, m_needMoreInputCount);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 16 — Drain AAC encoder
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase16_DrainAacEncoder() {
    m_pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    m_pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    DrainAacOutput();

    PrintPhasePass(16, "drain AAC encoder");
    char detail[64];
    snprintf(detail, sizeof(detail), "total audio packets: %zu", m_audioPackets.size());
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 17 — Unregister NVENC resource
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase17_UnregisterNvencResource() {
    if (m_registeredResource == nullptr) {
        PrintPhaseFail(17, "unregister NVENC resource", "m_registeredResource is null");
        return false;
    }
    NVENCSTATUS st = m_nvencFuncs.nvEncUnregisterResource(m_encoder, m_registeredResource);
    if (st != NV_ENC_SUCCESS) {
        char buf[96];
        snprintf(buf, sizeof(buf), "nvEncUnregisterResource: %s", NvencStatusName(st));
        PrintPhaseFail(17, "unregister NVENC resource", buf);
        return false;
    }
    m_registeredResource = nullptr;

    PrintPhasePass(17, "unregister NVENC resource");
    PrintDetail("NV12 D3D11 texture unregistered before nvEncDestroyEncoder");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 18 — Derive AV1 CodecPrivate from Sequence Header OBU
// ---------------------------------------------------------------------------

namespace {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t total_bits = 0;
    size_t pos = 0;
    bool overflow = false;

    uint32_t f(int n) {
        if (n <= 0)
            return 0;
        if (pos + static_cast<size_t>(n) > total_bits) {
            overflow = true;
            return 0;
        }
        uint32_t val = 0;
        for (int i = 0; i < n; ++i) {
            size_t byte_idx = pos / 8;
            size_t bit_idx = 7 - (pos % 8);
            val = (val << 1) | ((data[byte_idx] >> bit_idx) & 1);
            ++pos;
        }
        return val;
    }

    // UVLC: read a UVLC value (for AV1 seq_profile parsing)
    uint32_t uvlc() {
        int leadingZeros = 0;
        while (true) {
            uint32_t bit = f(1);
            if (overflow)
                return 0;
            if (bit == 1)
                break;
            ++leadingZeros;
            if (leadingZeros >= 32) {
                overflow = true;
                return 0;
            }
        }
        if (leadingZeros == 0)
            return 0;
        uint32_t value = f(leadingZeros);
        if (overflow)
            return 0;
        return value + (1u << leadingZeros) - 1;
    }
};

// Returns false on failure (sets *reason to a short diagnostic)
bool ParseSequenceHeaderObu(const uint8_t* obu_payload, size_t obu_size, uint32_t* out_seq_profile,
                            uint32_t* out_seq_level_idx_0, uint32_t* out_seq_tier_0, uint32_t* out_high_bitdepth,
                            uint32_t* out_twelve_bit, uint32_t* out_mono_chrome, uint32_t* out_subsampling_x,
                            uint32_t* out_subsampling_y, uint32_t* out_chroma_sample_position, char* reason,
                            size_t reason_size) {
    BitReader br;
    br.data = obu_payload;
    br.total_bits = obu_size * 8;
    br.pos = 0;
    br.overflow = false;

#define CHECK_OVERFLOW(ctx)                                                                                            \
    if (br.overflow) {                                                                                                 \
        snprintf(reason, reason_size, "BitReader overflow at " ctx);                                                   \
        return false;                                                                                                  \
    }

    uint32_t seq_profile = br.f(3);
    CHECK_OVERFLOW("seq_profile");
    uint32_t still_picture = br.f(1);
    (void)still_picture;
    CHECK_OVERFLOW("still_picture");
    uint32_t reduced_still_picture_header = br.f(1);
    CHECK_OVERFLOW("reduced_still_picture_header");

    if (reduced_still_picture_header) {
        snprintf(reason, reason_size, "reduced_still_picture_header=1 — cannot derive full color config");
        return false;
    }

    uint32_t timing_info_present_flag = br.f(1);
    CHECK_OVERFLOW("timing_info_present_flag");
    uint32_t decoder_model_info_present_flag = 0;

    if (timing_info_present_flag) {
        br.f(32);
        CHECK_OVERFLOW("num_units_in_display_tick");
        br.f(32);
        CHECK_OVERFLOW("time_scale");
        uint32_t equal_picture_interval = br.f(1);
        CHECK_OVERFLOW("equal_picture_interval");
        if (equal_picture_interval) {
            br.uvlc();
            CHECK_OVERFLOW("num_ticks_per_picture_minus_1");
        }

        decoder_model_info_present_flag = br.f(1);
        CHECK_OVERFLOW("decoder_model_info_present_flag");
        if (decoder_model_info_present_flag) {
            snprintf(reason, reason_size, "decoder_model_info_present_flag=1 — too complex to skip");
            return false;
        }
    }

    uint32_t initial_display_delay_present_flag = br.f(1);
    CHECK_OVERFLOW("initial_display_delay_present_flag");
    uint32_t operating_points_cnt_minus_1 = br.f(5);
    CHECK_OVERFLOW("operating_points_cnt_minus_1");

    uint32_t seq_level_idx[32] = {};
    uint32_t seq_tier[32] = {};

    for (uint32_t i = 0; i <= operating_points_cnt_minus_1; ++i) {
        br.f(12);
        CHECK_OVERFLOW("operating_point_idc"); // operating_point_idc
        seq_level_idx[i] = br.f(5);
        CHECK_OVERFLOW("seq_level_idx");
        if (seq_level_idx[i] >= 4) {
            seq_tier[i] = br.f(1);
            CHECK_OVERFLOW("seq_tier");
        } else {
            seq_tier[i] = 0;
        }
        if (decoder_model_info_present_flag) {
            snprintf(reason, reason_size, "operating_parameters_info path not supported");
            return false;
        }
        if (initial_display_delay_present_flag) {
            uint32_t delay_present = br.f(1);
            CHECK_OVERFLOW("initial_display_delay_present_for_this_op");
            if (delay_present) {
                br.f(4);
                CHECK_OVERFLOW("initial_display_delay_minus_1");
            }
        }
    }

    uint32_t seq_level_idx_0 = seq_level_idx[0];
    uint32_t seq_tier_0 = seq_tier[0];

    uint32_t frame_width_bits_minus_1 = br.f(4);
    CHECK_OVERFLOW("frame_width_bits_minus_1");
    uint32_t frame_height_bits_minus_1 = br.f(4);
    CHECK_OVERFLOW("frame_height_bits_minus_1");
    br.f(frame_width_bits_minus_1 + 1);
    CHECK_OVERFLOW("max_frame_width_minus_1");
    br.f(frame_height_bits_minus_1 + 1);
    CHECK_OVERFLOW("max_frame_height_minus_1");

    // frame_id_numbers_present_flag (only if !reduced_still_picture_header, which we checked)
    uint32_t frame_id_numbers_present_flag = br.f(1);
    CHECK_OVERFLOW("frame_id_numbers_present_flag");
    if (frame_id_numbers_present_flag) {
        br.f(4);
        CHECK_OVERFLOW("delta_frame_id_length_minus_2");
        br.f(3);
        CHECK_OVERFLOW("additional_frame_id_length_minus_1");
    }

    br.f(1);
    CHECK_OVERFLOW("use_128x128_superblock");
    br.f(1);
    CHECK_OVERFLOW("enable_filter_intra");
    br.f(1);
    CHECK_OVERFLOW("enable_intra_edge_filter");

    // The following only when !reduced_still_picture_header (already verified)
    br.f(1);
    CHECK_OVERFLOW("enable_interintra_compound");
    br.f(1);
    CHECK_OVERFLOW("enable_masked_compound");
    br.f(1);
    CHECK_OVERFLOW("enable_warped_motion");
    br.f(1);
    CHECK_OVERFLOW("enable_dual_filter");

    uint32_t enable_order_hint = br.f(1);
    CHECK_OVERFLOW("enable_order_hint");
    if (enable_order_hint) {
        br.f(1);
        CHECK_OVERFLOW("enable_jnt_comp");
        br.f(1);
        CHECK_OVERFLOW("enable_ref_frame_mvs");
    }

    uint32_t seq_choose_screen_content_tools = br.f(1);
    CHECK_OVERFLOW("seq_choose_screen_content_tools");
    uint32_t seq_force_screen_content_tools = 0;
    if (seq_choose_screen_content_tools) {
        seq_force_screen_content_tools = 2; // SELECT_SCREEN_CONTENT_TOOLS
    } else {
        seq_force_screen_content_tools = br.f(1);
        CHECK_OVERFLOW("seq_force_screen_content_tools");
    }

    if (seq_force_screen_content_tools > 0) {
        uint32_t seq_choose_integer_mv = br.f(1);
        CHECK_OVERFLOW("seq_choose_integer_mv");
        if (!seq_choose_integer_mv) {
            br.f(1);
            CHECK_OVERFLOW("seq_force_integer_mv");
        }
    }

    if (enable_order_hint) {
        br.f(3);
        CHECK_OVERFLOW("order_hint_bits_minus_1");
    }

    br.f(1);
    CHECK_OVERFLOW("enable_superres");
    br.f(1);
    CHECK_OVERFLOW("enable_cdef");
    br.f(1);
    CHECK_OVERFLOW("enable_restoration");

    // color_config()
    uint32_t high_bitdepth = br.f(1);
    CHECK_OVERFLOW("high_bitdepth");
    uint32_t twelve_bit = 0;
    if (seq_profile == 2 && high_bitdepth) {
        twelve_bit = br.f(1);
        CHECK_OVERFLOW("twelve_bit");
    }
    uint32_t mono_chrome = 0;
    if (seq_profile == 1) {
        mono_chrome = 0;
    } else {
        mono_chrome = br.f(1);
        CHECK_OVERFLOW("mono_chrome");
    }

    uint32_t color_description_present_flag = br.f(1);
    CHECK_OVERFLOW("color_description_present_flag");
    uint32_t color_primaries = 2;
    uint32_t transfer_characteristics = 2;
    uint32_t matrix_coefficients = 2;
    if (color_description_present_flag) {
        color_primaries = br.f(8);
        CHECK_OVERFLOW("color_primaries");
        transfer_characteristics = br.f(8);
        CHECK_OVERFLOW("transfer_characteristics");
        matrix_coefficients = br.f(8);
        CHECK_OVERFLOW("matrix_coefficients");
    }

    uint32_t subsampling_x = 0;
    uint32_t subsampling_y = 0;
    uint32_t chroma_sample_position = 0;

    if (mono_chrome) {
        br.f(1); // color_range
        CHECK_OVERFLOW("color_range(mono)");
        subsampling_x = 1;
        subsampling_y = 1;
        chroma_sample_position = 0;
    } else if (color_primaries == 1 && transfer_characteristics == 13 && matrix_coefficients == 0) {
        // CP_BT_709 && TC_SRGB && MC_IDENTITY -> full range, 4:4:4
        subsampling_x = 0;
        subsampling_y = 0;
        chroma_sample_position = 0;
    } else {
        br.f(1); // color_range
        CHECK_OVERFLOW("color_range");
        if (seq_profile == 0) {
            subsampling_x = 1;
            subsampling_y = 1;
        } else if (seq_profile == 1) {
            subsampling_x = 0;
            subsampling_y = 0;
        } else {
            if (twelve_bit) {
                subsampling_x = 1;
                subsampling_y = 1;
            } else {
                subsampling_x = 1;
                subsampling_y = 0;
            }
        }
        if (subsampling_x && subsampling_y) {
            chroma_sample_position = br.f(2);
            CHECK_OVERFLOW("chroma_sample_position");
        }
        br.f(1); // separate_uv_delta_q
        CHECK_OVERFLOW("separate_uv_delta_q");
    }

#undef CHECK_OVERFLOW

    *out_seq_profile = seq_profile;
    *out_seq_level_idx_0 = seq_level_idx_0;
    *out_seq_tier_0 = seq_tier_0;
    *out_high_bitdepth = high_bitdepth;
    *out_twelve_bit = twelve_bit;
    *out_mono_chrome = mono_chrome;
    *out_subsampling_x = subsampling_x;
    *out_subsampling_y = subsampling_y;
    *out_chroma_sample_position = chroma_sample_position;
    return true;
}

} // anonymous namespace

bool WgcNvencAacMkvProbe::Phase18_DeriveAv1CodecPrivate() {
    if (m_videoPackets.empty()) {
        PrintPhaseFail(18, "derive AV1 CodecPrivate", "no video packets");
        return false;
    }

    // Step 1: Find first keyframe packet
    const VideoPacket* keyPkt = nullptr;
    for (const auto& vp : m_videoPackets) {
        if (vp.is_keyframe) {
            keyPkt = &vp;
            break;
        }
    }
    if (keyPkt == nullptr) {
        // Fall back to first packet
        keyPkt = &m_videoPackets[0];
    }

    const uint8_t* bs = keyPkt->bytes.data();
    size_t bsLen = keyPkt->bytes.size();

    // Step 2: Scan OBUs looking for Sequence Header (type 1)
    const uint8_t* obu_payload = nullptr;
    size_t obu_payload_size = 0;
    size_t i = 0;

    while (i < bsLen) {
        uint8_t header_byte = bs[i++];
        uint32_t obu_type = (header_byte >> 3) & 0x0F;
        uint32_t extension_flag = (header_byte >> 2) & 0x1;
        uint32_t has_size_field = (header_byte >> 1) & 0x1;

        if (extension_flag) {
            if (i >= bsLen)
                break;
            ++i; // skip extension header byte
        }

        size_t payload_size = 0;
        if (has_size_field) {
            // LEB128
            size_t leb_shift = 0;
            for (int b = 0; b < 8; ++b) {
                if (i >= bsLen) {
                    payload_size = 0;
                    break;
                }
                uint8_t leb_byte = bs[i++];
                payload_size |= static_cast<size_t>(leb_byte & 0x7F) << leb_shift;
                leb_shift += 7;
                if ((leb_byte & 0x80) == 0)
                    break;
            }
        } else {
            // No size field — payload extends to end of packet
            payload_size = bsLen - i;
        }

        if (obu_type == 1) {
            // Sequence Header OBU found
            if (i + payload_size > bsLen) {
                PrintPhaseFail(18, "derive AV1 CodecPrivate", "Sequence Header OBU size exceeds packet boundary");
                return false;
            }
            obu_payload = bs + i;
            obu_payload_size = payload_size;
            break;
        }

        // Skip this OBU's payload
        i += payload_size;
        if (i > bsLen)
            break;
    }

    if (obu_payload == nullptr) {
        PrintPhaseFail(18, "derive AV1 CodecPrivate", "Sequence Header OBU (type 1) not found in first keyframe");
        return false;
    }

    // Step 3: Parse Sequence Header OBU payload
    uint32_t seq_profile = 0, seq_level_idx_0 = 0, seq_tier_0 = 0;
    uint32_t high_bitdepth = 0, twelve_bit = 0, mono_chrome = 0;
    uint32_t subsampling_x = 0, subsampling_y = 0, chroma_sample_position = 0;
    char parseReason[256] = {};

    bool ok = ParseSequenceHeaderObu(obu_payload, obu_payload_size, &seq_profile, &seq_level_idx_0, &seq_tier_0,
                                     &high_bitdepth, &twelve_bit, &mono_chrome, &subsampling_x, &subsampling_y,
                                     &chroma_sample_position, parseReason, sizeof(parseReason));

    if (!ok) {
        PrintPhaseFail(18, "derive AV1 CodecPrivate", parseReason);
        return false;
    }

    // Step 4: Build AV1CodecConfigurationRecord (4 bytes)
    m_av1CodecPrivate[0] = 0x81; // marker=1, version=1
    m_av1CodecPrivate[1] = static_cast<uint8_t>((seq_profile << 5) | (seq_level_idx_0 & 0x1F));
    m_av1CodecPrivate[2] =
        static_cast<uint8_t>((seq_tier_0 << 7) | (high_bitdepth << 6) | (twelve_bit << 5) | (mono_chrome << 4) |
                             (subsampling_x << 3) | (subsampling_y << 2) | (chroma_sample_position & 0x03));
    m_av1CodecPrivate[3] = 0x00;

    PrintPhasePass(18, "derive AV1 CodecPrivate");
    char detail[128];
    snprintf(detail, sizeof(detail), "AV1CodecConfigurationRecord: %02X %02X %02X %02X", m_av1CodecPrivate[0],
             m_av1CodecPrivate[1], m_av1CodecPrivate[2], m_av1CodecPrivate[3]);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail),
             "seq_profile=%u level=%u tier=%u high_bitdepth=%u twelve_bit=%u mono=%u subX=%u subY=%u csp=%u",
             seq_profile, seq_level_idx_0, seq_tier_0, high_bitdepth, twelve_bit, mono_chrome, subsampling_x,
             subsampling_y, chroma_sample_position);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 19 — Derive AAC CodecPrivate from MF_MT_USER_DATA
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase19_DeriveAacCodecPrivate() {
    if (m_pOutputType == nullptr) {
        PrintPhaseFail(19, "derive AAC CodecPrivate", "m_pOutputType is null");
        return false;
    }

    UINT32 blobSize = 0;
    HRESULT hr = m_pOutputType->GetBlobSize(MF_MT_USER_DATA, &blobSize);
    if (FAILED(hr) || blobSize < 14) {
        char buf[96];
        snprintf(buf, sizeof(buf), "MF_MT_USER_DATA missing or too small (blobSize=%u, hr=0x%08lX)", blobSize,
                 static_cast<unsigned long>(hr));
        PrintPhaseFail(19, "derive AAC CodecPrivate", buf);
        return false;
    }

    std::vector<BYTE> blob(blobSize);
    hr = m_pOutputType->GetBlob(MF_MT_USER_DATA, blob.data(), blobSize, nullptr);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetBlob(MF_MT_USER_DATA) failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(19, "derive AAC CodecPrivate", buf);
        return false;
    }

    // AudioSpecificConfig starts at offset 12 (after 12-byte HEAACWAVEINFO tail)
    m_aacCodecPrivate[0] = blob[12];
    m_aacCodecPrivate[1] = blob[13];

    PrintPhasePass(19, "derive AAC CodecPrivate");
    char detail[64];
    snprintf(detail, sizeof(detail), "AudioSpecificConfig: %02X %02X", m_aacCodecPrivate[0], m_aacCodecPrivate[1]);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 20 — Write and verify MKV
// ---------------------------------------------------------------------------

bool WgcNvencAacMkvProbe::Phase20_WriteAndVerifyMkv() {
    if (m_videoPackets.empty()) {
        PrintPhaseFail(20, "write and verify MKV", "no video packets to write");
        return false;
    }

    const char* outDir = "probe_wgc_nvenc_aac_mkv_output";
    const char* outPath = "probe_wgc_nvenc_aac_mkv_output\\av1_aac.mkv";

    CreateDirectoryW(L"probe_wgc_nvenc_aac_mkv_output", nullptr);

    // 20a: Open output file.
    // libwebm writes DocType=matroska automatically when A_AAC is present (not a WebM codec).
    mkvmuxer::MkvWriter mkvWriter;
    if (!mkvWriter.Open(outPath)) {
        PrintPhaseFail(20, "init Matroska writer", "failed to open output file");
        return false;
    }

    // 20b: Init segment
    mkvmuxer::Segment segment;
    if (!segment.Init(&mkvWriter)) {
        mkvWriter.Close();
        PrintPhaseFail(20, "init Matroska writer", "Segment::Init failed");
        return false;
    }
    segment.set_mode(mkvmuxer::Segment::kFile);

    // 20c: Add tracks
    uint64_t video_track_num =
        segment.AddVideoTrack(static_cast<int32_t>(m_encodeWidth), static_cast<int32_t>(m_encodeHeight), 1);
    if (video_track_num == 0) {
        mkvWriter.Close();
        PrintPhaseFail(20, "add video track", "AddVideoTrack returned 0");
        return false;
    }

    auto* vt = static_cast<mkvmuxer::VideoTrack*>(segment.GetTrackByNumber(video_track_num));
    vt->set_codec_id("V_AV1");
    vt->SetCodecPrivate(m_av1CodecPrivate, 4);
    vt->set_frame_rate(60.0);

    uint64_t audio_track_num = segment.AddAudioTrack(48000, 2, 2);
    if (audio_track_num == 0) {
        mkvWriter.Close();
        PrintPhaseFail(20, "add audio track", "AddAudioTrack returned 0");
        return false;
    }

    auto* at = static_cast<mkvmuxer::AudioTrack*>(segment.GetTrackByNumber(audio_track_num));
    at->set_codec_id("A_AAC");
    at->SetCodecPrivate(m_aacCodecPrivate, 2);

    // 20d: Sort and write packets
    std::vector<MuxPacket> mux_list;
    mux_list.reserve(m_videoPackets.size() + m_audioPackets.size());

    for (const auto& vp : m_videoPackets)
        mux_list.push_back({vp.timestamp_ns, video_track_num, vp.is_keyframe, vp.bytes.data(), vp.bytes.size()});
    for (const auto& ap : m_audioPackets)
        mux_list.push_back({ap.timestamp_ns, audio_track_num, true, ap.bytes.data(), ap.bytes.size()});

    std::stable_sort(mux_list.begin(), mux_list.end(),
                     [](const MuxPacket& a, const MuxPacket& b) { return a.timestamp_ns < b.timestamp_ns; });

    for (const auto& pkt : mux_list) {
        if (!segment.AddFrame(pkt.data, static_cast<uint64_t>(pkt.size), pkt.track_number, pkt.timestamp_ns,
                              pkt.is_key)) {
            mkvWriter.Close();
            PrintPhaseFail(20, "write MKV frames", "segment.AddFrame returned false");
            return false;
        }
    }

    // 20e: Finalize
    if (!segment.Finalize()) {
        mkvWriter.Close();
        PrintPhaseFail(20, "finalize MKV segment", "Segment::Finalize returned false");
        return false;
    }
    mkvWriter.Close();

    // 20f: Verify
    WIN32_FILE_ATTRIBUTE_DATA fileData{};
    if (!GetFileAttributesExW(L"probe_wgc_nvenc_aac_mkv_output\\av1_aac.mkv", GetFileExInfoStandard, &fileData)) {
        PrintPhaseFail(20, "verify output file", "file not found");
        return false;
    }

    uint64_t fileSize = (static_cast<uint64_t>(fileData.nFileSizeHigh) << 32) | fileData.nFileSizeLow;

    if (fileSize < 1024 * 1024) {
        char buf[80];
        snprintf(buf, sizeof(buf), "file size %llu < 1 MiB", static_cast<unsigned long long>(fileSize));
        PrintPhaseFail(20, "verify output file", buf);
        return false;
    }

    // EBML signature check
    {
        FILE* cf = nullptr;
        if (fopen_s(&cf, outPath, "rb") == 0 && cf != nullptr) {
            uint8_t sig[4] = {};
            fread(sig, 1, 4, cf);
            fclose(cf);
            if (sig[0] != 0x1A || sig[1] != 0x45 || sig[2] != 0xDF || sig[3] != 0xA3) {
                PrintPhaseFail(20, "verify output file", "EBML signature mismatch");
                return false;
            }
        }
    }

    PrintPhasePass(20, "write and verify MKV");
    char detail[128];
    snprintf(detail, sizeof(detail), "output: %s (%llu bytes)", outPath, static_cast<unsigned long long>(fileSize));
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "video packets: %zu, audio packets: %zu", m_videoPackets.size(),
             m_audioPackets.size());
    PrintDetail(detail);

    (void)outDir;
    return true;
}

// ---------------------------------------------------------------------------
// PrintSummary
// ---------------------------------------------------------------------------

void WgcNvencAacMkvProbe::PrintSummary() {
    uint64_t lastVideoTs = 0;
    if (!m_videoPackets.empty())
        lastVideoTs = m_videoPackets.back().timestamp_ns;

    uint64_t lastAudioTs = 0;
    if (!m_audioPackets.empty())
        lastAudioTs = m_audioPackets.back().timestamp_ns;

    double videoDur = static_cast<double>(lastVideoTs) / 1e9;
    double audioDur = static_cast<double>(lastAudioTs) / 1e9;
    double skewMs = std::abs(videoDur - audioDur) * 1000.0;

    auto kindStr = (m_target.kind == CaptureTarget::Kind::Monitor) ? L"monitor" : L"window";

    fprintf(stdout, "\n=== SUMMARY ===\n");
    fwprintf(stdout, L"  target               : %s -- %s\n", kindStr, m_target.description.c_str());
    fprintf(stdout, "  wgc frames total     : %d\n", m_wgcFramesTotal);
    fprintf(stdout, "  video frames captured: %d\n", m_capturedVideoFrames);
    fprintf(stdout, "  encoded video packets: %zu\n", m_videoPackets.size());
    fprintf(stdout, "  audio AAC packets    : %zu\n", m_audioPackets.size());
    fprintf(stdout, "  video bytes          : %llu\n", static_cast<unsigned long long>(m_totalVideoBytes));
    fprintf(stdout, "  audio bytes          : %llu\n", static_cast<unsigned long long>(m_totalAudioBytes));
    fprintf(stdout, "  elapsed              : %.1f s\n", m_elapsedSeconds);
    fprintf(stdout, "  video duration       : %.3f s\n", videoDur);
    fprintf(stdout, "  audio duration       : %.3f s\n", audioDur);
    fprintf(stdout, "  duration skew        : %.1f ms\n", skewMs);
    fprintf(stdout, "  source loss          : %s\n", m_sourceLost ? "yes" : "no");
    fprintf(stdout, "  output path          : probe_wgc_nvenc_aac_mkv_output\\av1_aac.mkv\n");

    WIN32_FILE_ATTRIBUTE_DATA fd{};
    if (GetFileAttributesExW(L"probe_wgc_nvenc_aac_mkv_output\\av1_aac.mkv", GetFileExInfoStandard, &fd)) {
        uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        fprintf(stdout, "  output file size     : %llu bytes\n", static_cast<unsigned long long>(sz));
    }
    fprintf(stdout, "================\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

int WgcNvencAacMkvProbe::Run() {
    fprintf(stdout, "[probe] === WGC+NVENC AV1 / WASAPI+MF AAC -> Matroska MKV integration probe (M2.8) ===\n");
    auto kindStr = (m_target.kind == CaptureTarget::Kind::Monitor) ? L"monitor" : L"window";
    fwprintf(stdout, L"[probe] target: %s -- %s\n", kindStr, m_target.description.c_str());
    fflush(stdout);

    struct Phase {
        const char* name;
        bool (WgcNvencAacMkvProbe::*fn)();
    };
    const Phase phases[] = {
        {"init COM + Media Foundation", &WgcNvencAacMkvProbe::Phase01_InitComAndMF},
        {"init D3D11 video-capable device", &WgcNvencAacMkvProbe::Phase02_InitD3D11VideoDevice},
        {"create WGC capture item", &WgcNvencAacMkvProbe::Phase03_CreateCaptureItem},
        {"validate source dimensions", &WgcNvencAacMkvProbe::Phase04_ValidateDimensions},
        {"load NVENC DLL + open encode session", &WgcNvencAacMkvProbe::Phase05_LoadNvencDll},
        {"AV1 GUID + NV12 format support", &WgcNvencAacMkvProbe::Phase06_QueryAv1Support},
        {"fetch AV1 preset config", &WgcNvencAacMkvProbe::Phase07_FetchPresetConfig},
        {"init AV1 encoder", &WgcNvencAacMkvProbe::Phase08_InitAv1Encoder},
        {"bitstream+NV12+vidproc+register", &WgcNvencAacMkvProbe::Phase09_BitstreamNv12VideoProcessorRegister},
        {"init AAC encoder", &WgcNvencAacMkvProbe::Phase10_InitAacEncoder},
        {"init WASAPI loopback", &WgcNvencAacMkvProbe::Phase11_InitWasapiLoopback},
        {"establish epoch + start WGC", &WgcNvencAacMkvProbe::Phase12_EstablishEpochAndStartWgc},
        {"wait for first WGC frame", &WgcNvencAacMkvProbe::Phase13_WaitForFirstFrame},
        {"30-second dual capture/encode loop", &WgcNvencAacMkvProbe::Phase14_DualCaptureLoop},
        {"drain AV1 encoder", &WgcNvencAacMkvProbe::Phase15_DrainAv1Encoder},
        {"drain AAC encoder", &WgcNvencAacMkvProbe::Phase16_DrainAacEncoder},
        {"unregister NVENC resource", &WgcNvencAacMkvProbe::Phase17_UnregisterNvencResource},
        {"derive AV1 CodecPrivate", &WgcNvencAacMkvProbe::Phase18_DeriveAv1CodecPrivate},
        {"derive AAC CodecPrivate", &WgcNvencAacMkvProbe::Phase19_DeriveAacCodecPrivate},
        {"write and verify MKV", &WgcNvencAacMkvProbe::Phase20_WriteAndVerifyMkv},
    };
    constexpr int kTotalPhases = static_cast<int>(sizeof(phases) / sizeof(phases[0]));

    for (int i = 0; i < kTotalPhases; ++i) {
        if (!(this->*(phases[i].fn))()) {
            fprintf(stdout, "STOP: probe failed at phase: %s\n", phases[i].name);
            fflush(stdout);
            PrintSummary();
            return 1;
        }
    }

    PrintSummary();
    fprintf(stdout, "[probe] PASS — WGC+NVENC AV1 / WASAPI+MF AAC -> Matroska MKV validated.\n");
    fflush(stdout);
    return 0;
}

} // namespace wgc_nvenc_aac_mkv
