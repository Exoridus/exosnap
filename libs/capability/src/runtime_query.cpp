// runtime_query.cpp — M3.3B runtime capability discovery
//
// Implements CapabilityBuilder::QueryRuntimeFacts().
//
// Probes performed:
//   A. NVENC: LoadLibraryW("nvEncodeAPI64.dll"), NvEncodeAPIGetMaxSupportedVersion
//   B. DXGI:  IDXGIFactory -> EnumAdapters -> adapter description string
//   C. Media Foundation AAC: MFTEnumEx + direct CLSID_AACMFTEncoder instantiation
//   D. OS: RtlGetVersion via ntdll.dll
//
// No NVENC sessions, no D3D11 devices, no codec GUID enumeration, no frame encoding.

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
// C. Media Foundation AAC runtime query
// -------------------------------------------------------------------------

void ProbeMfAac(MfAacRuntimeFacts& mf_aac) {
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
    ProbeAdapterName(snapshot.nvidia);
    ProbeMfAac(snapshot.mf_aac);
    ProbeOs(snapshot.os);
    ProbeDisplays(snapshot.displays);

    return snapshot;
}

} // namespace exosnap::capability
