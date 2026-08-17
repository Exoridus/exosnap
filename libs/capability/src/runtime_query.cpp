// runtime_query.cpp — M3.3B runtime capability discovery
//
// Implements CapabilityBuilder::QueryRuntimeFacts().
//
// Probes performed:
//   A.  NVENC presence: LoadLibraryW("nvEncodeAPI64.dll"), NvEncodeAPIGetMaxSupportedVersion
//   A2. NVENC per-GPU codec GUIDs: open a frameless NVENC session on an NVIDIA D3D11
//       device and enumerate EncodeGUIDs (best-effort, dev-verify-only — see below)
//   B.  DXGI:  IDXGIFactory -> EnumAdapters -> adapter description string
//   D.  OS: RtlGetVersion via ntdll.dll
//
// AAC audio capability is not probed here: since ADR 0052, AAC is encoded by
// FFmpeg's bundled native AAC-LC encoder (libs/recorder_core/src/ffmpeg_aac_encoder.cpp),
// which ships with every build and requires no Media Foundation component. It
// is therefore always available; see CapabilityBuilder::BuildStaticValidatedBaseline().
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

#include <recorder_core/hdr_color_space.h> // one definition of "HDR is on"

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

#include <capability/adapter_enum.h>
#include <capability/capability_builder.h>
#include <capability/runtime_snapshot.h>

#include <cstdio>
#include <string>
#include <unordered_map>
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

            // Per-codec 4:4:4 (YUV444, 8-bit) support via
            // NV_ENC_CAPS_SUPPORT_YUV444_ENCODE. Only queried for the codecs the
            // GPU actually advertised; a failed/absent query leaves the flag
            // false (no 4:4:4 claimed beyond the static baseline).
            if (funcs.nvEncGetEncodeCaps != nullptr) {
                auto query_yuv444 = [&funcs, encoder](const GUID& codec) -> bool {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS && value != 0;
                };
                if (nvidia.nvenc_h264) {
                    nvidia.nvenc_yuv444_h264 = query_yuv444(NV_ENC_CODEC_H264_GUID);
                }
                if (nvidia.nvenc_hevc) {
                    nvidia.nvenc_yuv444_hevc = query_yuv444(NV_ENC_CODEC_HEVC_GUID);
                }

                auto query_cap = [&funcs, encoder](const GUID& codec, NV_ENC_CAPS cap) -> int {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = cap;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS ? value : 0;
                };
                auto query_advanced = [&query_cap](const GUID& codec) -> NvencAdvancedEncodeFacts {
                    NvencAdvancedEncodeFacts adv;
                    adv.max_bframes = query_cap(codec, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                    adv.bframe_ref_mode = query_cap(codec, NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE);
                    adv.lookahead = query_cap(codec, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
                    adv.temporal_aq = query_cap(codec, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
                    return adv;
                };
                if (nvidia.nvenc_h264) {
                    nvidia.nvenc_adv_h264 = query_advanced(NV_ENC_CODEC_H264_GUID);
                }
                if (nvidia.nvenc_hevc) {
                    nvidia.nvenc_adv_hevc = query_advanced(NV_ENC_CODEC_HEVC_GUID);
                }
                if (nvidia.nvenc_av1) {
                    nvidia.nvenc_adv_av1 = query_advanced(NV_ENC_CODEC_AV1_GUID);
                }
            }
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

std::string Utf8From(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// -------------------------------------------------------------------------
// B2. Per-display HDR facts (IDXGIOutput6::GetDesc1)
// -------------------------------------------------------------------------
// The DisplayConfig facts DXGI cannot answer, keyed by GDI device name
// (e.g. "\\.\DISPLAY7", matching DXGI_OUTPUT_DESC1::DeviceName). Mirrors the
// QueryTargetFactsByGdiName join pattern in DisplayIdentityEnumerator.cpp: walk
// QueryDisplayConfig's active paths, resolve each path's source GDI name, then
// read that path's target for the friendly name and the advanced-colour state.
//
// The friendly name is read here rather than derived anywhere else because this is
// the one place where Windows itself pairs the two names for the SAME path — the
// join key the Qt-side screen list needs. Both fields are best-effort: a target or
// ACM read that fails leaves its own field at its default and never borrows the
// other's value.
struct DisplayConfigFacts {
    std::wstring friendly_name;
    bool wide_color_enforced = false;
};

std::unordered_map<std::wstring, DisplayConfigFacts> QueryDisplayConfigFactsByGdiName() {
    std::unordered_map<std::wstring, DisplayConfigFacts> out;

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return out;
    }
    paths.resize(path_count);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }

        DisplayConfigFacts facts;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            facts.friendly_name = target.monitorFriendlyDeviceName;
        }

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO acm{};
        acm.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        acm.header.size = sizeof(acm);
        acm.header.adapterId = path.targetInfo.adapterId;
        acm.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&acm.header) == ERROR_SUCCESS) {
            facts.wide_color_enforced = acm.wideColorEnforced != 0;
        }

        out.emplace(std::wstring(source.viewGdiDeviceName), std::move(facts));
    }
    return out;
}

void ProbeDisplays(std::vector<DisplayHdrFacts>& displays) {
    const std::unordered_map<std::wstring, DisplayConfigFacts> config_by_gdi_name = QueryDisplayConfigFactsByGdiName();

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
                    facts.hdr_active = recorder_core::IsHdrColorSpace(d.ColorSpace);
                    facts.bits_per_color = d.BitsPerColor;
                    facts.red_primary_x = d.RedPrimary[0];
                    facts.red_primary_y = d.RedPrimary[1];
                    facts.green_primary_x = d.GreenPrimary[0];
                    facts.green_primary_y = d.GreenPrimary[1];
                    facts.blue_primary_x = d.BluePrimary[0];
                    facts.blue_primary_y = d.BluePrimary[1];
                    facts.white_point_x = d.WhitePoint[0];
                    facts.white_point_y = d.WhitePoint[1];
                    facts.max_luminance_nits = d.MaxLuminance;
                    facts.min_luminance_nits = d.MinLuminance;
                    facts.max_full_frame_nits = d.MaxFullFrameLuminance;
                    const auto config_it = config_by_gdi_name.find(d.DeviceName);
                    if (config_it != config_by_gdi_name.end()) {
                        facts.wide_color_enforced = config_it->second.wide_color_enforced;
                        facts.friendly_name = Utf8From(config_it->second.friendly_name.c_str());
                    }
                    displays.push_back(std::move(facts));
                }
            }
            output.Reset();
        }
        adapter.Reset();
    }
}

// -------------------------------------------------------------------------
// B3. Cheap adapter-identity read (LUID + WDDM driver version)
// -------------------------------------------------------------------------
//
// Used only to build the disk-cache warm-start key (capability_cache_key.h);
// never consulted by the real probe path. Reuses EnumerateAdapters() (the
// same "skip software/WARP" filtering ProbeAdapterName applies) instead of
// re-walking DXGI adapters a third way.
void ProbeAdapterIdentity(AdapterIdentity& identity) {
    const std::vector<AdapterInfo> adapters = EnumerateAdapters();
    if (adapters.empty())
        return; // no real adapter — identity stays default (luid=0, driver_version empty)

    identity.adapter_luid = adapters.front().luid;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
        return; // driver_version stays empty — cache key still usable (less precise)

    LUID luid{};
    luid.LowPart = static_cast<DWORD>(static_cast<uint64_t>(identity.adapter_luid) & 0xFFFFFFFFu);
    luid.HighPart = static_cast<LONG>(static_cast<uint64_t>(identity.adapter_luid) >> 32);

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        return;

    // IDXGIAdapter::CheckInterfaceSupport is deprecated for feature queries
    // post-Direct3D 10, but it still reliably reports the WDDM user-mode
    // driver version as a packed LARGE_INTEGER on real hardware drivers.
    // DXGI_ERROR_UNSUPPORTED (e.g. some virtualized/basic drivers) leaves
    // driver_version empty — a graceful degrade, not a hard failure.
    LARGE_INTEGER umd_version{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd_version))) {
        const uint64_t v = static_cast<uint64_t>(umd_version.QuadPart);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu.%llu.%llu.%llu", static_cast<unsigned long long>((v >> 48) & 0xFFFFu),
                      static_cast<unsigned long long>((v >> 32) & 0xFFFFu),
                      static_cast<unsigned long long>((v >> 16) & 0xFFFFu),
                      static_cast<unsigned long long>(v & 0xFFFFu));
        identity.driver_version = buf;
    }
}

// -------------------------------------------------------------------------
// C0. Media Foundation presence pre-check (used by Cw below)
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
    // No AAC probe: FFmpeg's bundled native AAC-LC encoder is always available (ADR 0052).
    ProbeOs(snapshot.os);
    ProbeDisplays(snapshot.displays);

    return snapshot;
}

std::vector<DisplayHdrFacts> CapabilityBuilder::QueryDisplayFacts() {
    std::vector<DisplayHdrFacts> displays;
    ProbeDisplays(displays);
    return displays;
}

AdapterIdentity CapabilityBuilder::QueryAdapterIdentity() {
    AdapterIdentity identity;
    ProbeAdapterIdentity(identity);
    return identity;
}

} // namespace exosnap::capability
