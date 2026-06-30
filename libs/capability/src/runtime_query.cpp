// runtime_query.cpp — M3.3B runtime capability discovery
//
// Implements CapabilityBuilder::QueryRuntimeFacts().
//
// Probes performed:
//   A.  NVENC presence: LoadLibraryW("nvEncodeAPI64.dll"), NvEncodeAPIGetMaxSupportedVersion
//   A2. NVENC per-GPU codec GUIDs: open a frameless NVENC session on an NVIDIA D3D11
//       device and enumerate EncodeGUIDs (best-effort, dev-verify-only — see below)
//   B.  DXGI:  IDXGIFactory -> EnumAdapters -> adapter description string
//   C.  Media Foundation AAC: MFTEnumEx + direct CLSID_AACMFTEncoder instantiation
//   D.  OS: RtlGetVersion via ntdll.dll
//
// Probe A2 opens a real NVENC session (no frames are ever encoded). It is best-effort:
// any failure (no NVENC DLL, no NVIDIA device, no session, header missing in a headless
// build) leaves nvenc_codec_probed=false so the static baseline stands and headless CI
// never regresses to "no codecs". Its live per-codec result requires a physical NVIDIA
// GPU and therefore cannot be verified headlessly.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// DXGI
#include <dxgi.h>
#include <dxgi1_6.h> // IDXGIOutput6::GetDesc1 (per-display HDR facts)

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

// COM smart pointer support
#include <wrl/client.h>

// RtlGetVersion structure (available via ntdll without requiring WDK)
#include <winternl.h>

// NVENC per-GPU codec-GUID probe (A2). Guarded so a headless build without the vendor
// header (third_party/nvidia/nvEncodeAPI.h) still compiles and degrades gracefully.
#if __has_include(<nvEncodeAPI.h>)
#include <d3d11.h>
#include <nvEncodeAPI.h>
#define EXOSNAP_CAPABILITY_HAVE_NVENC 1
#else
#define EXOSNAP_CAPABILITY_HAVE_NVENC 0
#endif

#include <capability/capability_builder.h>
#include <capability/runtime_snapshot.h>

#include <cstdio>
#include <string>
#include <vector>

// NVENC API version function pointer type.
// The function signature: NvAPI_Status NvEncodeAPIGetMaxSupportedVersion(uint32_t* version)
// Returns 0 (NV_ENC_SUCCESS) on success.
using NvEncodeAPIGetMaxSupportedVersion_t = uint32_t(__stdcall*)(uint32_t*);

// RtlGetVersion function pointer type (ntdll export, always available on Windows 8+).
using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

namespace exosnap::capability {
namespace {

// -------------------------------------------------------------------------
// A. NVIDIA NVENC runtime query
// -------------------------------------------------------------------------

void ProbeNvidia(NvidiaRuntimeFacts& nvidia) {
    HMODULE nvenc_module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!nvenc_module) {
        nvidia.nvenc_dll_present = false;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "LoadLibraryW(nvEncodeAPI64.dll) failed, GetLastError=%lu",
                      static_cast<unsigned long>(GetLastError()));
        nvidia.failure_detail = buf;
        return;
    }

    nvidia.nvenc_dll_present = true;

    // Resolve the version query function.
    auto fn = reinterpret_cast<NvEncodeAPIGetMaxSupportedVersion_t>(
        GetProcAddress(nvenc_module, "NvEncodeAPIGetMaxSupportedVersion"));

    if (!fn) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "GetProcAddress(NvEncodeAPIGetMaxSupportedVersion) failed, GetLastError=%lu",
                      static_cast<unsigned long>(GetLastError()));
        nvidia.failure_detail = buf;
        FreeLibrary(nvenc_module);
        return;
    }

    // Call the version function. Returns 0 (NV_ENC_SUCCESS) on success.
    uint32_t api_version = 0;
    const uint32_t nvenc_result = fn(&api_version);
    if (nvenc_result == 0u) {
        nvidia.nvenc_api_version_valid = true;
        nvidia.nvenc_api_version = api_version;
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "NvEncodeAPIGetMaxSupportedVersion returned non-zero status: %u",
                      static_cast<unsigned int>(nvenc_result));
        nvidia.failure_detail = buf;
    }

    FreeLibrary(nvenc_module);
}

// -------------------------------------------------------------------------
// A2. NVENC per-GPU codec-GUID probe (best-effort; dev-verify-only)
// -------------------------------------------------------------------------

#if EXOSNAP_CAPABILITY_HAVE_NVENC

// Create a D3D11 device on the first NVIDIA adapter (PCI vendor 0x10DE). Returns
// nullptr when there is no NVIDIA adapter or device creation fails. Best-effort.
Microsoft::WRL::ComPtr<ID3D11Device> CreateNvidiaD3D11Device() {
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10DEu) {
            adapter.Reset();
            continue;
        }
        ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // DRIVER_TYPE_UNKNOWN is required when an explicit adapter is supplied.
        const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                             ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        adapter.Reset();
        if (SUCCEEDED(hr) && device) {
            return device;
        }
    }
    return nullptr;
}

void ProbeNvencCodecs(NvidiaRuntimeFacts& nvidia) {
    // No point opening a session if the lightweight presence probe already failed.
    if (!nvidia.nvenc_dll_present || !nvidia.nvenc_api_version_valid) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device = CreateNvidiaD3D11Device();
    if (!device) {
        return; // no NVIDIA D3D11 device — keep nvenc_codec_probed = false
    }

    HMODULE dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll) {
        return;
    }

    using CreateInstance_t = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto pCreate = reinterpret_cast<CreateInstance_t>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    if (!pCreate) {
        FreeLibrary(dll);
        return;
    }

    NV_ENCODE_API_FUNCTION_LIST funcs{};
    funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (pCreate(&funcs) != NV_ENC_SUCCESS || funcs.nvEncOpenEncodeSessionEx == nullptr ||
        funcs.nvEncGetEncodeGUIDCount == nullptr || funcs.nvEncGetEncodeGUIDs == nullptr) {
        FreeLibrary(dll);
        return;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device.Get();
    params.apiVersion = NVENCAPI_VERSION;

    void* encoder = nullptr;
    if (funcs.nvEncOpenEncodeSessionEx(&params, &encoder) != NV_ENC_SUCCESS || encoder == nullptr) {
        if (encoder != nullptr && funcs.nvEncDestroyEncoder != nullptr) {
            funcs.nvEncDestroyEncoder(encoder);
        }
        FreeLibrary(dll);
        return;
    }

    uint32_t count = 0;
    if (funcs.nvEncGetEncodeGUIDCount(encoder, &count) == NV_ENC_SUCCESS && count > 0) {
        std::vector<GUID> guids(count);
        uint32_t got = 0;
        if (funcs.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &got) == NV_ENC_SUCCESS) {
            for (uint32_t i = 0; i < got; ++i) {
                if (IsEqualGUID(guids[i], NV_ENC_CODEC_AV1_GUID) != 0) {
                    nvidia.nvenc_av1 = true;
                } else if (IsEqualGUID(guids[i], NV_ENC_CODEC_HEVC_GUID) != 0) {
                    nvidia.nvenc_hevc = true;
                } else if (IsEqualGUID(guids[i], NV_ENC_CODEC_H264_GUID) != 0) {
                    nvidia.nvenc_h264 = true;
                }
            }
            // Only now is the per-codec result authoritative.
            nvidia.nvenc_codec_probed = true;
        }
    }

    if (funcs.nvEncDestroyEncoder != nullptr) {
        funcs.nvEncDestroyEncoder(encoder);
    }
    FreeLibrary(dll);
}

#else // EXOSNAP_CAPABILITY_HAVE_NVENC

// Headless / no-vendor-header build: probe is unavailable. Leaves nvenc_codec_probed
// false so the static baseline stands.
void ProbeNvencCodecs(NvidiaRuntimeFacts&) {
}

#endif // EXOSNAP_CAPABILITY_HAVE_NVENC

// -------------------------------------------------------------------------
// B. DXGI adapter name discovery
// -------------------------------------------------------------------------

void ProbeAdapterName(NvidiaRuntimeFacts& nvidia) {
    // DXGI is always available on Windows 11+, so load via the static import.
    // We use the static link (dxgi.lib) rather than dynamic LoadLibrary to
    // avoid a use-after-free: ComPtr destructors would call Release() through
    // a vtable that lives in the DLL, which must not be freed while COM objects
    // are still alive.

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        // Not a critical failure; adapter_name stays empty.
        return;
    }

    // Enumerate adapters and pick the first discrete one that is not the
    // Microsoft Basic Render Driver (software adapter).
    // Fall back to adapter 0 if no discrete adapter is found.
    std::string best_name;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(i, adapter.GetAddressOf()))) {
            break;
        }
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc))) {
            continue;
        }
        // Convert wide description to narrow UTF-8 using WideCharToMultiByte.
        const int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string name(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), len, nullptr, nullptr);

            // Prefer first non-software adapter.
            const bool is_software = (desc.VendorId == 0x1414u && desc.DeviceId == 0x008cu); // WARP / Microsoft Basic
            if (best_name.empty() || (!is_software && best_name.find("Microsoft Basic") != std::string::npos)) {
                best_name = std::move(name);
            }
        }
    }

    if (!best_name.empty()) {
        nvidia.adapter_name = std::move(best_name);
    }
}

// -------------------------------------------------------------------------
// B2. Per-display HDR facts (IDXGIOutput6::GetDesc1)
// -------------------------------------------------------------------------
void ProbeDisplays(std::vector<DisplayHdrFacts>& displays) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return; // non-critical; displays stays empty
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            Microsoft::WRL::ComPtr<IDXGIOutput6> out6;
            if (SUCCEEDED(output.As(&out6))) {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(out6->GetDesc1(&d))) {
                    DisplayHdrFacts facts;
                    const int len = WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 1) {
                        facts.name.resize(static_cast<size_t>(len - 1));
                        WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, facts.name.data(), len, nullptr, nullptr);
                    }
                    // HDR is ON when the output is in a PQ/BT.2020 colour space.
                    facts.hdr_active = (d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                        d.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020);
                    facts.bits_per_color = d.BitsPerColor;
                    facts.max_luminance_nits = d.MaxLuminance;
                    facts.min_luminance_nits = d.MinLuminance;
                    facts.max_full_frame_nits = d.MaxFullFrameLuminance;
                    displays.push_back(std::move(facts));
                }
            }
            output.Reset();
        }
        adapter.Reset();
    }
}

// -------------------------------------------------------------------------
// C0. Media Foundation presence pre-check (shared by C and Cw below)
// -------------------------------------------------------------------------

// Check whether mfplat.dll is loadable without triggering a delay-load
// exception. This is safe to call regardless of delay-load state.
static bool IsMfPlatPresent() noexcept {
    HMODULE h = LoadLibraryW(L"mfplat.dll");
    if (h) {
        FreeLibrary(h);
        return true;
    }
    return false;
}

// -------------------------------------------------------------------------
// Cw. Media Foundation webcam probe (S4)
// -------------------------------------------------------------------------

void ProbeMfWebcam(MfWebcamRuntimeFacts& mf_webcam) {
    // Webcam capture requires IMFSourceReader (mfreadwrite.dll) which itself
    // depends on mfplat.dll. A LoadLibraryW probe on mfplat.dll is sufficient:
    // if it is absent the entire Media Foundation stack is absent (Windows-N
    // without the Media Feature Pack).
    if (IsMfPlatPresent()) {
        mf_webcam.available = true;
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "LoadLibraryW(mfplat.dll) failed, GetLastError=%lu",
                      static_cast<unsigned long>(GetLastError()));
        mf_webcam.failure_detail = buf;
        mf_webcam.available = false;
    }
}

// -------------------------------------------------------------------------
// C. Media Foundation AAC runtime query
// -------------------------------------------------------------------------

void ProbeMfAac(MfAacRuntimeFacts& mf_aac) {
    // Pre-check: if mfplat.dll is absent we must not attempt any MF API call —
    // the delay-load stub would raise a VcppException (SEH) instead of returning
    // a graceful HRESULT. Return early with the same "unavailable" result.
    if (!IsMfPlatPresent()) {
        mf_aac.failure_detail = "mfplat.dll not found — Media Foundation not installed on this system.";
        return;
    }
    // Initialize COM on this thread in a multi-threaded apartment.
    //
    // CoInitializeEx return value semantics:
    //   S_OK              — COM was not yet initialized on this thread; this call
    //                       initialized it and incremented the reference count.
    //   S_FALSE           — COM was already initialized in a compatible apartment;
    //                       the reference count was still incremented.
    //   RPC_E_CHANGED_MODE — COM was already initialized in an incompatible apartment;
    //                        the reference count was NOT incremented.
    //   Other failure     — initialization failed entirely; count not incremented.
    //
    // CoUninitialize must be called if and only if CoInitializeEx returned S_OK or
    // S_FALSE (i.e., SUCCEEDED(co_hr)).  Using SUCCEEDED rather than (co_hr == S_OK)
    // is deliberate: both success codes leave a reference that must be released.
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(co_hr);

    // Start Media Foundation (safe to call multiple times; internally ref-counted).
    const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    const bool mf_started = SUCCEEDED(mf_hr);

    // --- Step 1: MFTEnumEx query ---
    if (mf_started) {
        // Target: audio encoder outputting AAC (MFAudioFormat_AAC).
        MFT_REGISTER_TYPE_INFO output_type{};
        output_type.guidMajorType = MFMediaType_Audio;
        output_type.guidSubtype = MFAudioFormat_AAC;

        IMFActivate** activate_array = nullptr;
        UINT32 count = 0;

        const HRESULT enum_hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL,
                                          nullptr, // any input type
                                          &output_type, &activate_array, &count);

        if (SUCCEEDED(enum_hr)) {
            mf_aac.mftenum_found = (count > 0);
            // Release each IMFActivate and free the array.
            for (UINT32 i = 0; i < count; ++i) {
                if (activate_array[i]) {
                    activate_array[i]->Release();
                }
            }
            if (activate_array) {
                CoTaskMemFree(activate_array);
            }
        }
        // If MFTEnumEx itself fails, mftenum_found stays false — non-fatal.
    }

    // --- Step 2: Direct CLSID_AACMFTEncoder instantiation ---
    // This is the M2.7 fallback: enumeration may return zero even when the
    // encoder is present; direct CoCreateInstance succeeds in that scenario.
    {
        // CLSID_AACMFTEncoder = {32D186A7-218F-4C75-8876-DD77273A8999}
        static const CLSID kClsidAacMftEncoder = {
            0x32D186A7u, 0x218Fu, 0x4C75u, {0x88u, 0x76u, 0xDDu, 0x77u, 0x27u, 0x3Au, 0x89u, 0x99u}};

        Microsoft::WRL::ComPtr<IUnknown> aac_encoder;
        const HRESULT clsid_hr = CoCreateInstance(kClsidAacMftEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
                                                  reinterpret_cast<void**>(aac_encoder.GetAddressOf()));

        mf_aac.clsid_instantiable = SUCCEEDED(clsid_hr);
    }

    // Populate failure_detail only when both paths fail.
    if (!mf_aac.available()) {
        mf_aac.failure_detail = "MFTEnumEx found no AAC encoders and direct CLSID_AACMFTEncoder "
                                "instantiation failed. Media Foundation AAC encoder is not available "
                                "on this system.";
    }

    // Shutdown in reverse order.
    if (mf_started) {
        MFShutdown();
    }
    if (co_initialized) {
        CoUninitialize();
    }
}

// -------------------------------------------------------------------------
// D. OS version / build query
// -------------------------------------------------------------------------

void ProbeOs(OsRuntimeFacts& os) {
    // Use RtlGetVersion via dynamic load so it bypasses the compatibility shim
    // that VerifyVersionInfo/GetVersionEx applies. RtlGetVersion always returns
    // the real OS version.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        os.failure_detail = "GetModuleHandleW(ntdll.dll) failed; cannot query OS version.";
        return;
    }

    auto RtlGetVersionFn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!RtlGetVersionFn) {
        os.failure_detail = "GetProcAddress(RtlGetVersion) failed; OS version unavailable.";
        return;
    }

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const LONG status = RtlGetVersionFn(&info);
    if (status != 0) { // STATUS_SUCCESS = 0
        char buf[64];
        std::snprintf(buf, sizeof(buf), "RtlGetVersion returned NTSTATUS 0x%08lX",
                      static_cast<unsigned long>(static_cast<ULONG>(status)));
        os.failure_detail = buf;
        return;
    }

    os.build_number = static_cast<uint32_t>(info.dwBuildNumber);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", static_cast<unsigned long>(info.dwMajorVersion),
                  static_cast<unsigned long>(info.dwMinorVersion), static_cast<unsigned long>(info.dwBuildNumber));
    os.version_string = buf;
}

} // namespace

// -------------------------------------------------------------------------
// Public entrypoint
// -------------------------------------------------------------------------

RuntimeCapabilitySnapshot CapabilityBuilder::QueryRuntimeFacts() {
    RuntimeCapabilitySnapshot snapshot;

    // Probes are independent; each writes only to its own sub-struct.
    ProbeNvidia(snapshot.nvidia);
    ProbeNvencCodecs(snapshot.nvidia); // A2: per-GPU codec GUIDs (best-effort; needs a real NVIDIA GPU)
    ProbeAdapterName(snapshot.nvidia);
    ProbeMfWebcam(snapshot.mf_webcam); // S4: webcam MF presence probe (safe, LoadLibraryW-based)
    ProbeMfAac(snapshot.mf_aac);       // guarded internally by IsMfPlatPresent()
    ProbeOs(snapshot.os);
    ProbeDisplays(snapshot.displays);

    return snapshot;
}

} // namespace exosnap::capability
