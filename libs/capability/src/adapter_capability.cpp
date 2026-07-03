// adapter_capability.cpp — per-adapter encoder capability probe.
//
// NVIDIA: reuses the NVENC encode-GUID probe technique from runtime_query.cpp
// (A2), but targeted at ONE specific adapter (matched by LUID) instead of
// "the first NVIDIA adapter DXGI happens to enumerate first". This matters on
// multi-NVIDIA-GPU systems (e.g. a laptop with an NVIDIA dGPU plus an eGPU, or
// a workstation with two NVIDIA cards) where the two adapters can genuinely
// differ in codec support across driver/silicon generations.
//
// AMD / Intel / Other: no probe is attempted. ExoSnap's MVP only has an NVENC
// encoder backend wired (see README "Hardware encoder: NVIDIA NVENC only");
// AMD/AMF and Intel/QSV are roadmap items with no implementation to probe
// against, so this deliberately returns probed=false with an honest message
// rather than fabricating a static guess.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi.h>

#include <wrl/client.h>

#include <capability/adapter_capability.h>

#include <cstdint>
#include <vector>

// Guarded exactly like runtime_query.cpp: a headless build tree without the
// vendor header (third_party/nvidia/nvEncodeAPI.h) still compiles and
// degrades gracefully to probed=false.
#if __has_include(<nvEncodeAPI.h>)
#include <d3d11.h>
#include <nvEncodeAPI.h>
#define EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC 1
#else
#define EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC 0
#endif

namespace exosnap::capability {
namespace {

constexpr const char* kNotYetSupportedMessage =
    "encoder backend not yet supported (no AMD/AMF, Intel/QSV, or software encoder is wired in this build)";

#if EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC

using NvEncodeAPICreateInstance_t = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

// Re-enumerates DXGI to find the IDXGIAdapter1 whose LUID matches `target_luid`
// (as packed by PackAdapterLuid in adapter_enum.h), then creates a D3D11
// device on it. Returns nullptr if no adapter matches or device creation fails.
Microsoft::WRL::ComPtr<ID3D11Device> CreateD3D11DeviceForLuid(int64_t target_luid) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;

    ComPtr<IDXGIAdapter1> adapter;
    // SUCCEEDED() (not != DXGI_ERROR_NOT_FOUND) so ANY failure — end-of-list or a
    // real error — terminates the loop instead of spinning on a failing call.
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;
        if (PackAdapterLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart) != target_luid)
            continue;

        ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // DRIVER_TYPE_UNKNOWN is required when an explicit adapter is supplied.
        const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
                                             ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (SUCCEEDED(hr) && device)
            return device;
        return nullptr; // matching adapter found but device creation failed — do not fall back to another adapter
    }
    return nullptr; // no adapter with this LUID (should not happen: caller enumerated it moments ago)
}

// Opens a real (frameless) NVENC session on `device` via the already-loaded
// NVENC DLL and enumerates its EncodeGUIDs. Mirrors runtime_query.cpp's
// ProbeNvencCodecs, but writes straight into an AdapterEncoderCapability
// rather than NvidiaRuntimeFacts. `dll` stays owned by the caller.
void ProbeNvencGuidsOnDevice(HMODULE dll, ID3D11Device* device, AdapterEncoderCapability& out) {
    auto pCreate = reinterpret_cast<NvEncodeAPICreateInstance_t>(GetProcAddress(dll, "NvEncodeAPICreateInstance"));
    if (!pCreate)
        return;

    NV_ENCODE_API_FUNCTION_LIST funcs{};
    funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (pCreate(&funcs) != NV_ENC_SUCCESS || funcs.nvEncOpenEncodeSessionEx == nullptr ||
        funcs.nvEncGetEncodeGUIDCount == nullptr || funcs.nvEncGetEncodeGUIDs == nullptr) {
        return;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device;
    params.apiVersion = NVENCAPI_VERSION;

    void* encoder = nullptr;
    if (funcs.nvEncOpenEncodeSessionEx(&params, &encoder) != NV_ENC_SUCCESS || encoder == nullptr) {
        if (encoder != nullptr && funcs.nvEncDestroyEncoder != nullptr)
            funcs.nvEncDestroyEncoder(encoder);
        return;
    }

    uint32_t count = 0;
    if (funcs.nvEncGetEncodeGUIDCount(encoder, &count) == NV_ENC_SUCCESS && count > 0) {
        std::vector<GUID> guids(count);
        uint32_t got = 0;
        if (funcs.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &got) == NV_ENC_SUCCESS) {
            for (uint32_t i = 0; i < got; ++i) {
                if (IsEqualGUID(guids[i], NV_ENC_CODEC_AV1_GUID) != 0)
                    out.av1 = true;
                else if (IsEqualGUID(guids[i], NV_ENC_CODEC_HEVC_GUID) != 0)
                    out.hevc = true;
                else if (IsEqualGUID(guids[i], NV_ENC_CODEC_H264_GUID) != 0)
                    out.h264 = true;
            }
            out.probed = true; // only now is the per-codec result authoritative
        }
    }

    if (funcs.nvEncDestroyEncoder != nullptr)
        funcs.nvEncDestroyEncoder(encoder);
}

AdapterEncoderCapability ProbeNvidiaAdapter(const AdapterInfo& adapter) {
    AdapterEncoderCapability out;
    out.backend_label = "NVENC";

    // Load the NVENC DLL exactly once for presence check AND the GUID probe.
    HMODULE dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll) {
        out.provenance = "NVENC driver not detected (nvEncodeAPI64.dll not found)";
        return out;
    }

    auto device = CreateD3D11DeviceForLuid(adapter.luid);
    if (!device) {
        out.provenance = "NVENC probe unavailable (could not create a D3D11 device on this adapter)";
        FreeLibrary(dll);
        return out;
    }

    ProbeNvencGuidsOnDevice(dll, device.Get(), out);
    out.provenance =
        out.probed ? "probed via NVENC encode GUIDs" : "NVENC probe unavailable (session open or GUID query failed)";
    // Free the DLL only after the D3D11 device is still alive but the encoder
    // session is closed (ProbeNvencGuidsOnDevice destroyed it before returning).
    FreeLibrary(dll);
    return out;
}

#else // EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC

// Headless / no-vendor-header build: the probe path is unavailable. Still
// reports the backend as NVENC (this IS an NVIDIA adapter) but honestly
// unprobed, matching runtime_query.cpp's degrade-gracefully behavior.
AdapterEncoderCapability ProbeNvidiaAdapter(const AdapterInfo&) {
    AdapterEncoderCapability out;
    out.backend_label = "NVENC";
    out.provenance = "NVENC probe unavailable (vendor SDK header not present in this build)";
    return out;
}

#endif // EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC

} // namespace

std::string EncoderBackendLabelForVendor(AdapterVendor vendor) noexcept {
    return vendor == AdapterVendor::Nvidia ? std::string("NVENC") : std::string();
}

AdapterEncoderCapability ProbeAdapterEncoderCapability(const AdapterInfo& adapter) {
    if (adapter.vendor != AdapterVendor::Nvidia) {
        AdapterEncoderCapability out;
        out.probed = false;
        out.backend_label.clear();
        out.provenance = kNotYetSupportedMessage;
        return out;
    }
    return ProbeNvidiaAdapter(adapter);
}

} // namespace exosnap::capability
