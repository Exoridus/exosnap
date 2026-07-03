// adapter_enum.cpp — DXGI multi-adapter enumeration (additive to the existing
// single-resolved CapabilitySet in capability_set.h / capability_builder.cpp).
//
// The existing global capability probe (runtime_query.cpp ProbeAdapterName /
// CreateNvidiaD3D11Device) only ever looks at ONE adapter — the first
// non-software adapter DXGI returns for ProbeAdapterName, and the first
// vendor-0x10DE adapter for the NVENC codec-GUID probe. On a single-GPU
// system (by far the common case) that is indistinguishable from "the right
// adapter"; on a multi-GPU system it silently ignores every adapter after the
// first match. This file enumerates ALL adapters so the Device page can show
// every one of them; it does not change what the global CapabilitySet uses.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi.h>

#include <wrl/client.h>

#include <capability/adapter_enum.h>

namespace exosnap::capability {
namespace {

// Microsoft Basic Render Driver (WARP / software rasterizer) — never a real
// encode target, so it is excluded from the enumerated list.
constexpr uint32_t kMicrosoftVendorId = 0x1414u;
constexpr uint32_t kBasicRenderDeviceId = 0x008cu;

std::string NarrowFromWide(const wchar_t* wide) {
    if (wide == nullptr)
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

AdapterVendor ClassifyVendor(uint32_t vendor_id) noexcept {
    switch (vendor_id) {
    case 0x10DEu:
        return AdapterVendor::Nvidia;
    case 0x1002u: // AMD/ATI
    case 0x1022u: // AMD (some APU SKUs report the CPU vendor ID on the iGPU function)
        return AdapterVendor::Amd;
    case 0x8086u:
        return AdapterVendor::Intel;
    default:
        return AdapterVendor::Other;
    }
}

AdapterKind ClassifyKind(uint64_t dedicated_video_memory_bytes, uint64_t shared_system_memory_bytes) noexcept {
    if (dedicated_video_memory_bytes == 0 && shared_system_memory_bytes == 0)
        return AdapterKind::Unknown;
    // Integrated GPUs report a small-to-zero dedicated VRAM pool and rely on
    // shared system memory; discrete GPUs report a large dedicated pool that
    // dominates (or at least matches) the shared pool. 512 MiB is a
    // conservative floor: every discrete card shipped in the last decade
    // reports well above it, while iGPU dedicated apertures are typically in
    // the 0-256 MiB range.
    constexpr uint64_t kDiscreteFloorBytes = 512ull * 1024 * 1024;
    if (dedicated_video_memory_bytes >= kDiscreteFloorBytes)
        return AdapterKind::Discrete;
    return AdapterKind::Integrated;
}

std::vector<AdapterInfo> EnumerateAdapters() {
    std::vector<AdapterInfo> adapters;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return adapters; // no DXGI — return empty rather than throw

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    // SUCCEEDED() (not != DXGI_ERROR_NOT_FOUND) so ANY failure — end-of-list or a
    // real error — terminates the loop instead of spinning on a failing call.
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;

        const bool is_software = (desc.VendorId == kMicrosoftVendorId && desc.DeviceId == kBasicRenderDeviceId) ||
                                 (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (is_software)
            continue;

        AdapterInfo info;
        info.name = NarrowFromWide(desc.Description);
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.vendor = ClassifyVendor(desc.VendorId);
        info.luid = PackAdapterLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
        info.dedicated_video_memory_bytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
        info.shared_system_memory_bytes = static_cast<uint64_t>(desc.SharedSystemMemory);
        info.kind = ClassifyKind(info.dedicated_video_memory_bytes, info.shared_system_memory_bytes);

        adapters.push_back(std::move(info));
    }

    return adapters;
}

} // namespace exosnap::capability
