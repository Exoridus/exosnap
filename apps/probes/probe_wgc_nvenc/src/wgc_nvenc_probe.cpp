#include "wgc_nvenc_probe.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wgc_nvenc {

namespace {

constexpr int kPadColumn = 46;

void PrintPhasePass(int phase, const char* label) {
    char prefix[96];
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
    char prefix[96];
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
// EnumerateTargets (adapted from probe_wgc_preview)
// ---------------------------------------------------------------------------

std::vector<CaptureTarget> WgcNvencProbe::EnumerateTargets() {
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

WgcNvencProbe::WgcNvencProbe(const CaptureTarget& target) : m_target(target) {
}

WgcNvencProbe::~WgcNvencProbe() {
    CleanupCapture();
    CleanupEncoder();
    m_stagingTex = nullptr;
    m_d3dContext = nullptr;
    m_d3dDevice = nullptr;
    if (m_nvencDll != nullptr) {
        FreeLibrary(m_nvencDll);
        m_nvencDll = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Cleanup helpers
// ---------------------------------------------------------------------------

void WgcNvencProbe::CleanupCapture() {
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

void WgcNvencProbe::CleanupEncoder() {
    if (m_encoder != nullptr && m_nvencFuncs.nvEncDestroyInputBuffer != nullptr && m_inputBuffer != nullptr) {
        m_nvencFuncs.nvEncDestroyInputBuffer(m_encoder, m_inputBuffer);
        m_inputBuffer = nullptr;
    }
    if (m_encoder != nullptr && m_nvencFuncs.nvEncDestroyBitstreamBuffer != nullptr && m_bitstreamBuffer != nullptr) {
        m_nvencFuncs.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstreamBuffer);
        m_bitstreamBuffer = nullptr;
    }
    if (m_encoder != nullptr && m_nvencFuncs.nvEncDestroyEncoder != nullptr) {
        m_nvencFuncs.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
}

// ---------------------------------------------------------------------------
// BGRA → NV12 conversion (BT.601 limited range, integer arithmetic)
// ---------------------------------------------------------------------------

void WgcNvencProbe::ConvertBgraToNv12(const uint8_t* bgraSrc, uint32_t srcPitch, uint8_t* nv12Dst, uint32_t dstPitch,
                                      uint32_t width, uint32_t height) {
    uint8_t* yPlane = nv12Dst;
    uint8_t* uvPlane = nv12Dst + static_cast<size_t>(dstPitch) * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = bgraSrc + static_cast<size_t>(srcPitch) * y;
        uint8_t* yRow = yPlane + static_cast<size_t>(dstPitch) * y;

        // Y plane: one luma sample per pixel
        for (uint32_t x = 0; x < width; ++x) {
            int b = srcRow[x * 4 + 0];
            int g = srcRow[x * 4 + 1];
            int r = srcRow[x * 4 + 2];
            int yv = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[x] = static_cast<uint8_t>(std::clamp(yv, 16, 235));
        }

        // UV plane: one Cb/Cr pair per 2×2 block, computed on even rows
        if ((y & 1u) == 0u) {
            const uint8_t* srcRow2 = (y + 1 < height) ? bgraSrc + static_cast<size_t>(srcPitch) * (y + 1) : srcRow;
            uint8_t* uvRow = uvPlane + static_cast<size_t>(dstPitch) * (y >> 1);

            for (uint32_t x = 0; x < width; x += 2) {
                // 2×2 block samples
                int b00 = srcRow[x * 4 + 0], g00 = srcRow[x * 4 + 1], r00 = srcRow[x * 4 + 2];
                int b10 = (x + 1 < width) ? (int)srcRow[(x + 1) * 4 + 0] : b00;
                int g10 = (x + 1 < width) ? (int)srcRow[(x + 1) * 4 + 1] : g00;
                int r10 = (x + 1 < width) ? (int)srcRow[(x + 1) * 4 + 2] : r00;
                int b01 = srcRow2[x * 4 + 0], g01 = srcRow2[x * 4 + 1], r01 = srcRow2[x * 4 + 2];
                int b11 = (x + 1 < width) ? (int)srcRow2[(x + 1) * 4 + 0] : b01;
                int g11 = (x + 1 < width) ? (int)srcRow2[(x + 1) * 4 + 1] : g01;
                int r11 = (x + 1 < width) ? (int)srcRow2[(x + 1) * 4 + 2] : r01;

                int ravg = (r00 + r10 + r01 + r11 + 2) >> 2;
                int gavg = (g00 + g10 + g01 + g11 + 2) >> 2;
                int bavg = (b00 + b10 + b01 + b11 + 2) >> 2;

                int cb = ((-38 * ravg - 74 * gavg + 112 * bavg + 128) >> 8) + 128;
                int cr = ((112 * ravg - 94 * gavg - 18 * bavg + 128) >> 8) + 128;

                uvRow[x + 0] = static_cast<uint8_t>(std::clamp(cb, 16, 240)); // U/Cb
                uvRow[x + 1] = static_cast<uint8_t>(std::clamp(cr, 16, 240)); // V/Cr
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 01 — Init D3D11 (BGRA_SUPPORT required by WGC)
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase01_InitD3D11() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, m_d3dDevice.put(), nullptr,
                                   m_d3dContext.put());
    if (FAILED(hr) || m_d3dDevice == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(1, "init D3D11 device", buf);
        return false;
    }
    PrintPhasePass(1, "init D3D11 device");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 02 — Create WGC capture item, read source size
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase02_CreateCaptureItem() {
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
        PrintPhaseFail(2, "create WGC capture item", buf);
        return false;
    } catch (...) {
        PrintPhaseFail(2, "create WGC capture item", "unknown exception");
        return false;
    }

    if (m_item == nullptr) {
        PrintPhaseFail(2, "create WGC capture item", "item is null after creation");
        return false;
    }

    auto sz = m_item.Size();
    m_sourceWidth = static_cast<uint32_t>(sz.Width);
    m_sourceHeight = static_cast<uint32_t>(sz.Height);

    PrintPhasePass(2, "create WGC capture item");
    char detail[64];
    snprintf(detail, sizeof(detail), "source size: %ux%u", m_sourceWidth, m_sourceHeight);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 03 — Validate and round source dimensions to even
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase03_ValidateDimensions() {
    m_encodeWidth = m_sourceWidth & ~1u;
    m_encodeHeight = m_sourceHeight & ~1u;

    if (m_encodeWidth < 2 || m_encodeHeight < 2) {
        char buf[80];
        snprintf(buf, sizeof(buf), "source %ux%u rounds to %ux%u — too small for NV12", m_sourceWidth, m_sourceHeight,
                 m_encodeWidth, m_encodeHeight);
        PrintPhaseFail(3, "validate source dimensions", buf);
        return false;
    }

    PrintPhasePass(3, "validate source dimensions");
    char detail[80];
    snprintf(detail, sizeof(detail), "encode size: %ux%u", m_encodeWidth, m_encodeHeight);
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 04 — Load NVENC DLL + create API instance
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase04_LoadNvencDll() {
    m_nvencDll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (m_nvencDll == nullptr) {
        char buf[80];
        snprintf(buf, sizeof(buf), "LoadLibraryW failed (GetLastError=%lu)",
                 static_cast<unsigned long>(GetLastError()));
        PrintPhaseFail(4, "load NVENC DLL + API instance", buf);
        return false;
    }

    using PFN = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<PFN>(GetProcAddress(m_nvencDll, "NvEncodeAPICreateInstance"));
    if (pCreate == nullptr) {
        PrintPhaseFail(4, "load NVENC DLL + API instance", "NvEncodeAPICreateInstance not exported");
        return false;
    }

    m_nvencFuncs = {};
    m_nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = pCreate(&m_nvencFuncs);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(4, "load NVENC DLL + API instance", NvencStatusName(st));
        return false;
    }

    PrintPhasePass(4, "load NVENC DLL + API instance");
    char detail[80];
    snprintf(detail, sizeof(detail), "NVENCAPI_VERSION major=%u minor=%u",
             static_cast<unsigned>(NVENCAPI_MAJOR_VERSION), static_cast<unsigned>(NVENCAPI_MINOR_VERSION));
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 05 — Open NVENC encode session (shared D3D11 device)
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase05_OpenNvencSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = m_d3dDevice.get();
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_nvencFuncs.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS || m_encoder == nullptr) {
        PrintPhaseFail(5, "open NVENC encode session", NvencStatusName(st));
        return false;
    }
    PrintPhasePass(5, "open NVENC encode session");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 06 — Query AV1 GUID + NV12 format support
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase06_QueryAv1Support() {
    // AV1 GUID
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
                       "NV_ENC_CODEC_AV1_GUID not in supported encode GUIDs"
                       " (hardware: Ada Lovelace / RTX 40-series or newer required)");
        return false;
    }

    // NV12 format
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

bool WgcNvencProbe::Phase07_FetchPresetConfig() {
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
    return true;
}

// ---------------------------------------------------------------------------
// Phase 08 — Init AV1 encoder at WGC source dimensions
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase08_InitAv1Encoder() {
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
// Phase 09 — Create NVENC input / bitstream buffers
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase09_CreateNvencBuffers() {
    NV_ENC_CREATE_INPUT_BUFFER ibp{};
    ibp.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    ibp.width = m_encodeWidth;
    ibp.height = m_encodeHeight;
    ibp.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS st = m_nvencFuncs.nvEncCreateInputBuffer(m_encoder, &ibp);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(9, "create NVENC input/output buffers", NvencStatusName(st));
        return false;
    }
    m_inputBuffer = ibp.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER bsp{};
    bsp.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = m_nvencFuncs.nvEncCreateBitstreamBuffer(m_encoder, &bsp);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(9, "create NVENC input/output buffers", NvencStatusName(st));
        return false;
    }
    m_bitstreamBuffer = bsp.bitstreamBuffer;

    PrintPhasePass(9, "create NVENC input/output buffers");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 10 — Create staging texture (CPU-readable, BGRA8, same size as encoder)
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase10_CreateStagingTexture() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_encodeWidth;
    desc.Height = m_encodeHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, m_stagingTex.put());
    if (FAILED(hr) || m_stagingTex == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "CreateTexture2D (staging) failed 0x%08lX", static_cast<unsigned long>(hr));
        PrintPhaseFail(10, "create staging texture", buf);
        return false;
    }
    PrintPhasePass(10, "create staging texture");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 11 — Start WGC frame pool + capture session
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase11_StartWgcCapture() {
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
        PrintPhaseFail(11, "start WGC frame pool", buf);
        return false;
    } catch (...) {
        PrintPhaseFail(11, "start WGC frame pool", "unknown exception");
        return false;
    }

    PrintPhasePass(11, "start WGC frame pool");
    PrintDetail("frame pool: 3 buffers, B8G8R8A8UIntNormalized, polling mode");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 12 — Wait for first WGC frame (timeout 5 s), validate format/size
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase12_WaitForFirstFrame() {
    LARGE_INTEGER freq, tStart, tNow;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    constexpr double kTimeoutSec = 5.0;

    while (true) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_sourceLost) {
            PrintPhaseFail(12, "wait for first WGC frame", "source lost before first frame");
            return false;
        }

        QueryPerformanceCounter(&tNow);
        double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) / static_cast<double>(freq.QuadPart);
        if (elapsed > kTimeoutSec) {
            PrintPhaseFail(12, "wait for first WGC frame", "timeout (5 s) waiting for first frame");
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

            // Correction #1: validate format and dimensions of first frame
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);

            if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                char buf[96];
                snprintf(buf, sizeof(buf), "first frame format 0x%X — expected DXGI_FORMAT_B8G8R8A8_UNORM (0x%X)",
                         static_cast<unsigned>(desc.Format), static_cast<unsigned>(DXGI_FORMAT_B8G8R8A8_UNORM));
                PrintPhaseFail(12, "wait for first WGC frame", buf);
                return false;
            }
            if (desc.Width != m_encodeWidth || desc.Height != m_encodeHeight) {
                char buf[128];
                snprintf(buf, sizeof(buf), "first frame size %ux%u != encode size %ux%u", desc.Width, desc.Height,
                         m_encodeWidth, m_encodeHeight);
                PrintPhaseFail(12, "wait for first WGC frame", buf);
                return false;
            }

            PrintPhasePass(12, "wait for first WGC frame");
            char detail[80];
            snprintf(detail, sizeof(detail), "first frame: %ux%u BGRA8", desc.Width, desc.Height);
            PrintDetail(detail);
            return true;

        } catch (const winrt::hresult_error& e) {
            char buf[80];
            snprintf(buf, sizeof(buf), "TryGetNextFrame exception 0x%08X", static_cast<unsigned int>(e.code().value));
            PrintPhaseFail(12, "wait for first WGC frame", buf);
            return false;
        } catch (...) {
            PrintPhaseFail(12, "wait for first WGC frame", "unknown exception");
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 13 — Capture-convert-encode loop
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase13_CaptureEncodeLoop() {
    constexpr int kMaxFrames = 300;
    constexpr double kMaxSec = 5.0;

    LARGE_INTEGER freq, tStart, tNow;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    int frameIdx = 0;

    while (true) {
        // ---- Check termination conditions ----
        QueryPerformanceCounter(&tNow);
        double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) / static_cast<double>(freq.QuadPart);

        if (m_capturedFrames >= kMaxFrames) {
            m_terminationReason = TerminationReason::MaxFrames;
            break;
        }
        if (elapsed >= kMaxSec) {
            m_terminationReason = TerminationReason::TimeLimit;
            break;
        }
        if (m_sourceLost) {
            m_terminationReason = TerminationReason::SourceLoss;
            break;
        }

        // ---- Pump messages ----
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // ---- Drain WGC frames, keep latest ----
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame lastWgcFrame = nullptr;
        winrt::com_ptr<ID3D11Texture2D> latestTex;

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
                    lastWgcFrame = frame; // keep alive until CopyResource
                }
            }
        } catch (const winrt::hresult_error&) {
            // non-fatal, log skipped for brevity
        } catch (...) {
        }

        if (latestTex == nullptr) {
            Sleep(1);
            continue;
        }

        // ---- Correction #1: validate frame dimensions and format ----
        D3D11_TEXTURE2D_DESC texDesc{};
        latestTex->GetDesc(&texDesc);

        if (texDesc.Width != m_encodeWidth || texDesc.Height != m_encodeHeight) {
            char buf[128];
            snprintf(buf, sizeof(buf), "frame %d: dimension mismatch: WGC=%ux%u encoder=%ux%u", frameIdx, texDesc.Width,
                     texDesc.Height, m_encodeWidth, m_encodeHeight);
            PrintPhaseFail(13, "capture-convert-encode loop", buf);
            return false;
        }
        if (texDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            char buf[96];
            snprintf(buf, sizeof(buf), "frame %d: unexpected texture format 0x%X", frameIdx,
                     static_cast<unsigned>(texDesc.Format));
            PrintPhaseFail(13, "capture-convert-encode loop", buf);
            return false;
        }

        // ---- Copy GPU texture to staging (while WGC frame is alive) ----
        m_d3dContext->CopyResource(m_stagingTex.get(), latestTex.get());

        // Release WGC frame → returns buffer to pool
        latestTex = nullptr;
        lastWgcFrame = nullptr;

        // ---- Map staging texture ----
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_d3dContext->Map(m_stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "frame %d: Map(stagingTex) failed 0x%08lX", frameIdx,
                     static_cast<unsigned long>(hr));
            PrintPhaseFail(13, "capture-convert-encode loop", buf);
            return false;
        }

        // ---- Lock NVENC input buffer ----
        NV_ENC_LOCK_INPUT_BUFFER lockIn{};
        lockIn.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockIn.inputBuffer = m_inputBuffer;
        lockIn.doNotWait = 0;

        NVENCSTATUS nvSt = m_nvencFuncs.nvEncLockInputBuffer(m_encoder, &lockIn);
        if (nvSt != NV_ENC_SUCCESS) {
            // Correction #2: release staging map before returning
            m_d3dContext->Unmap(m_stagingTex.get(), 0);
            char buf[96];
            snprintf(buf, sizeof(buf), "frame %d: nvEncLockInputBuffer: %s", frameIdx, NvencStatusName(nvSt));
            PrintPhaseFail(13, "capture-convert-encode loop", buf);
            return false;
        }

        if (frameIdx == 0) {
            m_inputPitch = lockIn.pitch;
        }

        // ---- Convert BGRA → NV12 ----
        ConvertBgraToNv12(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                          static_cast<uint8_t*>(lockIn.bufferDataPtr), lockIn.pitch, m_encodeWidth, m_encodeHeight);

        // ---- Correction #2: release both resources on all paths ----
        m_nvencFuncs.nvEncUnlockInputBuffer(m_encoder, m_inputBuffer);
        m_d3dContext->Unmap(m_stagingTex.get(), 0);

        // ---- Submit to NVENC ----
        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = m_encodeWidth;
        pic.inputHeight = m_encodeHeight;
        pic.inputPitch = m_inputPitch;
        pic.inputBuffer = m_inputBuffer;
        pic.outputBitstream = m_bitstreamBuffer;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.encodePicFlags = 0;
        pic.inputTimeStamp = static_cast<uint64_t>(frameIdx);

        nvSt = m_nvencFuncs.nvEncEncodePicture(m_encoder, &pic);

        if (nvSt == NV_ENC_SUCCESS) {
            m_capturedFrames++;

            NV_ENC_LOCK_BITSTREAM lockBS{};
            lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
            lockBS.outputBitstream = m_bitstreamBuffer;
            lockBS.doNotWait = 0;

            nvSt = m_nvencFuncs.nvEncLockBitstream(m_encoder, &lockBS);
            if (nvSt != NV_ENC_SUCCESS) {
                char buf[96];
                snprintf(buf, sizeof(buf), "frame %d: nvEncLockBitstream: %s", frameIdx, NvencStatusName(nvSt));
                PrintPhaseFail(13, "capture-convert-encode loop", buf);
                return false;
            }

            m_totalEncodedBytes += lockBS.bitstreamSizeInBytes;
            m_encodedPackets++;

            EncodedPacket pkt;
            pkt.timestamp = lockBS.outputTimeStamp;
            pkt.bytes.assign(static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                             static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes);
            m_packets.push_back(std::move(pkt));

            NVENCSTATUS unlockSt = m_nvencFuncs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
            if (unlockSt != NV_ENC_SUCCESS) {
                char buf[96];
                snprintf(buf, sizeof(buf), "frame %d: nvEncUnlockBitstream: %s", frameIdx, NvencStatusName(unlockSt));
                PrintPhaseFail(13, "capture-convert-encode loop", buf);
                return false;
            }

        } else if (nvSt == NV_ENC_ERR_NEED_MORE_INPUT) {
            m_capturedFrames++;
            m_needMoreInputCount++;
        } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "frame %d: nvEncEncodePicture: %s", frameIdx, NvencStatusName(nvSt));
            PrintPhaseFail(13, "capture-convert-encode loop", buf);
            return false;
        }

        frameIdx++;

        if (m_capturedFrames > 0 && m_capturedFrames % 60 == 0) {
            int dropped = m_wgcFramesTotal - m_capturedFrames;
            char detail[128];
            snprintf(detail, sizeof(detail), "[frame %03d] wgc_total=%d captured=%d dropped=%d packets=%d bytes=%llu",
                     m_capturedFrames, m_wgcFramesTotal, m_capturedFrames, dropped, m_encodedPackets,
                     static_cast<unsigned long long>(m_totalEncodedBytes));
            PrintDetail(detail);
        }
    }

    QueryPerformanceCounter(&tNow);
    m_elapsedSeconds = static_cast<double>(tNow.QuadPart - tStart.QuadPart) / static_cast<double>(freq.QuadPart);

    // Correction #4: PASS if at least one frame submitted, no fatal error,
    //                expected termination reason
    if (m_capturedFrames == 0) {
        PrintPhaseFail(13, "capture-convert-encode loop", "no frames captured");
        return false;
    }

    PrintPhasePass(13, "capture-convert-encode loop");
    switch (m_terminationReason) {
    case TerminationReason::MaxFrames:
        PrintDetail("stopped: 300 frames captured");
        break;
    case TerminationReason::TimeLimit:
        PrintDetail("stopped: 5-second time limit reached");
        break;
    case TerminationReason::SourceLoss:
        PrintDetail("stopped: source loss");
        break;
    default:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase 14 — EOS flush + conservative post-EOS drain
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase14_EosFlushAndDrain() {
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVENCSTATUS st = m_nvencFuncs.nvEncEncodePicture(m_encoder, &eos);
    if (st != NV_ENC_SUCCESS) {
        PrintPhaseFail(14, "EOS flush and drain", NvencStatusName(st));
        return false;
    }

    // Conservative drain: attempt up to needMoreInputCount times;
    // stop early if lock fails — not a fatal error.
    for (int i = 0; i < m_needMoreInputCount; ++i) {
        NV_ENC_LOCK_BITSTREAM lockBS{};
        lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBS.outputBitstream = m_bitstreamBuffer;
        lockBS.doNotWait = 0;

        st = m_nvencFuncs.nvEncLockBitstream(m_encoder, &lockBS);
        if (st != NV_ENC_SUCCESS) {
            char note[96];
            snprintf(note, sizeof(note), "post-EOS drain stopped at attempt %d: %s", i + 1, NvencStatusName(st));
            PrintDetail(note);
            break;
        }

        m_totalEncodedBytes += lockBS.bitstreamSizeInBytes;
        m_encodedPackets++;

        EncodedPacket pkt;
        pkt.timestamp = lockBS.outputTimeStamp;
        pkt.bytes.assign(static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr),
                         static_cast<const uint8_t*>(lockBS.bitstreamBufferPtr) + lockBS.bitstreamSizeInBytes);
        m_packets.push_back(std::move(pkt));

        st = m_nvencFuncs.nvEncUnlockBitstream(m_encoder, m_bitstreamBuffer);
        if (st != NV_ENC_SUCCESS) {
            PrintPhaseFail(14, "EOS flush and drain", NvencStatusName(st));
            return false;
        }
    }

    PrintPhasePass(14, "EOS flush and drain");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 15 — Write IVF file
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase15_WriteIvfFile() {
    if (m_packets.empty()) {
        PrintPhaseFail(15, "write IVF file", "no packets to write");
        return false;
    }

    uint32_t ivfFrameCount = static_cast<uint32_t>(m_packets.size());

    if (!CreateDirectoryW(L"probe_wgc_nvenc_output", nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            char buf[64];
            snprintf(buf, sizeof(buf), "CreateDirectoryW failed: error %lu", static_cast<unsigned long>(err));
            PrintPhaseFail(15, "write IVF file", buf);
            return false;
        }
    }

    FILE* f = nullptr;
    errno_t e = fopen_s(&f, "probe_wgc_nvenc_output\\wgc_av1.ivf", "wb");
    if (e != 0 || f == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "fopen_s failed: errno %d", static_cast<int>(e));
        PrintPhaseFail(15, "write IVF file", buf);
        return false;
    }

    auto writeU16 = [](FILE* fp, uint16_t v) -> bool {
        uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        return fwrite(b, 1, 2, fp) == 2;
    };
    auto writeU32 = [](FILE* fp, uint32_t v) -> bool {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        return fwrite(b, 1, 4, fp) == 4;
    };
    auto writeU64 = [](FILE* fp, uint64_t v) -> bool {
        uint8_t b[8] = {uint8_t(v),       uint8_t(v >> 8),  uint8_t(v >> 16), uint8_t(v >> 24),
                        uint8_t(v >> 32), uint8_t(v >> 40), uint8_t(v >> 48), uint8_t(v >> 56)};
        return fwrite(b, 1, 8, fp) == 8;
    };

    // IVF file header (32 bytes)
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
        PrintPhaseFail(15, "write IVF file", "fwrite failed writing IVF file header");
        return false;
    }

    uint64_t bytesWritten = 32;
    for (const auto& pkt : m_packets) {
        ok = writeU32(f, static_cast<uint32_t>(pkt.bytes.size()));
        ok = ok && writeU64(f, pkt.timestamp);
        ok = ok && (fwrite(pkt.bytes.data(), 1, pkt.bytes.size(), f) == pkt.bytes.size());
        if (!ok) {
            fclose(f);
            PrintPhaseFail(15, "write IVF file", "fwrite failed writing IVF frame data");
            return false;
        }
        bytesWritten += 12 + pkt.bytes.size();
    }

    fflush(f);
    fclose(f);

    PrintPhasePass(15, "write IVF file");
    PrintDetail("output: probe_wgc_nvenc_output\\wgc_av1.ivf");
    char detail[96];
    snprintf(detail, sizeof(detail), "packets written   : %u", ivfFrameCount);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "ivf frame count   : %u", ivfFrameCount);
    PrintDetail(detail);
    snprintf(detail, sizeof(detail), "bytes written     : %llu", static_cast<unsigned long long>(bytesWritten));
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 16 — Verify IVF file (exact size check per correction #3)
// ---------------------------------------------------------------------------

bool WgcNvencProbe::Phase16_VerifyIvfFile() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(L"probe_wgc_nvenc_output\\wgc_av1.ivf", GetFileExInfoStandard, &data)) {
        PrintPhaseFail(16, "verify IVF file", "GetFileAttributesExW failed — file not found or inaccessible");
        return false;
    }

    uint64_t fileSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

    // Correction #3: compute exact expected size
    uint64_t expectedBytes = 32; // IVF file header
    for (const auto& pkt : m_packets) {
        expectedBytes += 12 + static_cast<uint64_t>(pkt.bytes.size());
    }

    if (fileSize <= 32) {
        char buf[80];
        snprintf(buf, sizeof(buf), "file size %llu <= IVF header size (32)", static_cast<unsigned long long>(fileSize));
        PrintPhaseFail(16, "verify IVF file", buf);
        return false;
    }

    constexpr uint64_t kMaxExpected = 50ULL * 1024 * 1024;
    if (fileSize > kMaxExpected) {
        char buf[80];
        snprintf(buf, sizeof(buf), "file size %llu bytes unexpectedly large (> 50 MiB)",
                 static_cast<unsigned long long>(fileSize));
        PrintPhaseFail(16, "verify IVF file", buf);
        return false;
    }

    if (fileSize != expectedBytes) {
        char buf[96];
        snprintf(buf, sizeof(buf), "IVF file size mismatch: expected %llu, got %llu",
                 static_cast<unsigned long long>(expectedBytes), static_cast<unsigned long long>(fileSize));
        PrintPhaseFail(16, "verify IVF file", buf);
        return false;
    }

    PrintPhasePass(16, "verify IVF file");
    PrintDetail("file: probe_wgc_nvenc_output\\wgc_av1.ivf");
    char detail[80];
    snprintf(detail, sizeof(detail), "size: %llu bytes (exact match)", static_cast<unsigned long long>(fileSize));
    PrintDetail(detail);
    return true;
}

// ---------------------------------------------------------------------------
// PrintSummary
// ---------------------------------------------------------------------------

void WgcNvencProbe::PrintSummary() {
    int dropped = m_wgcFramesTotal - m_capturedFrames;
    double fps = (m_elapsedSeconds > 0.0) ? (static_cast<double>(m_capturedFrames) / m_elapsedSeconds) : 0.0;
    bool loss = m_sourceLost;

    auto kindStr = (m_target.kind == CaptureTarget::Kind::Monitor) ? L"monitor" : L"window";
    fprintf(stdout, "\n=== SUMMARY ===\n");
    fwprintf(stdout, L"  target           : %s -- %s\n", kindStr, m_target.description.c_str());
    fprintf(stdout, "  wgc frames total : %d\n", m_wgcFramesTotal);
    fprintf(stdout, "  captured frames  : %d\n", m_capturedFrames);
    fprintf(stdout, "  dropped/skipped  : %d\n", dropped);
    fprintf(stdout, "  NEED_MORE_INPUT  : %d\n", m_needMoreInputCount);
    fprintf(stdout, "  encoded packets  : %d\n", m_encodedPackets);
    fprintf(stdout, "  total bytes      : %llu\n", static_cast<unsigned long long>(m_totalEncodedBytes));
    fprintf(stdout, "  elapsed          : %.3f s\n", m_elapsedSeconds);
    fprintf(stdout, "  encode fps       : %.1f fps\n", fps);
    fprintf(stdout, "  source loss      : %s\n", loss ? "yes" : "no");
    if (!m_packets.empty()) {
        fprintf(stdout, "  output           : probe_wgc_nvenc_output\\wgc_av1.ivf\n");
    }
    fprintf(stdout, "===============\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

int WgcNvencProbe::Run() {
    fprintf(stdout, "[probe] === WGC -> NVENC AV1 integration probe ===\n");
    auto kindStr = (m_target.kind == CaptureTarget::Kind::Monitor) ? L"monitor" : L"window";
    fwprintf(stdout, L"[probe] target: %s -- %s\n", kindStr, m_target.description.c_str());
    fflush(stdout);

    struct Phase {
        const char* name;
        bool (WgcNvencProbe::*fn)();
    };
    const Phase phases[] = {
        {"init D3D11 device", &WgcNvencProbe::Phase01_InitD3D11},
        {"create WGC capture item", &WgcNvencProbe::Phase02_CreateCaptureItem},
        {"validate source dimensions", &WgcNvencProbe::Phase03_ValidateDimensions},
        {"load NVENC DLL + API instance", &WgcNvencProbe::Phase04_LoadNvencDll},
        {"open NVENC encode session", &WgcNvencProbe::Phase05_OpenNvencSession},
        {"AV1 GUID + NV12 format support", &WgcNvencProbe::Phase06_QueryAv1Support},
        {"fetch AV1 preset config", &WgcNvencProbe::Phase07_FetchPresetConfig},
        {"init AV1 encoder", &WgcNvencProbe::Phase08_InitAv1Encoder},
        {"create NVENC input/output buffers", &WgcNvencProbe::Phase09_CreateNvencBuffers},
        {"create staging texture", &WgcNvencProbe::Phase10_CreateStagingTexture},
        {"start WGC frame pool", &WgcNvencProbe::Phase11_StartWgcCapture},
        {"wait for first WGC frame", &WgcNvencProbe::Phase12_WaitForFirstFrame},
        {"capture-convert-encode loop", &WgcNvencProbe::Phase13_CaptureEncodeLoop},
        {"EOS flush and drain", &WgcNvencProbe::Phase14_EosFlushAndDrain},
        {"write IVF file", &WgcNvencProbe::Phase15_WriteIvfFile},
        {"verify IVF file", &WgcNvencProbe::Phase16_VerifyIvfFile},
    };
    constexpr int kTotalPhases = static_cast<int>(sizeof(phases) / sizeof(phases[0]));

    for (int i = 0; i < kTotalPhases; ++i) {
        if (!(this->*(phases[i].fn))()) {
            fprintf(stdout, "STOP: probe did not pass at %s.\n", phases[i].name);
            fflush(stdout);
            PrintSummary();
            return 1;
        }
    }

    PrintSummary();
    fprintf(stdout, "[probe] encode probe PASS — WGC -> AV1 IVF pipeline validated.\n");
    fflush(stdout);
    return 0;
}

} // namespace wgc_nvenc
