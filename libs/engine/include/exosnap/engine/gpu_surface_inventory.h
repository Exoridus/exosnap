#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <exosnap/engine/performance_measurements.h>
#include <new>

namespace exosnap::engine {
enum class GpuSurfaceOwner {
    CaptureHeld,
    PacingRing,
    Compositor,
    WebcamCursor,
    Hdr,
    EncoderInputs,
    Preview,
    Editor,
    Readback,
    Count
};
inline constexpr std::array<const char*, static_cast<size_t>(GpuSurfaceOwner::Count)> kGpuSurfaceOwnerNames{
    "capture_held",   "pacing_ring", "compositor", "webcam_cursor", "hdr",
    "encoder_inputs", "preview",     "editor",     "readback"};
struct GpuSurfaceUsage {
    uint64_t bytes = 0;
    uint64_t surfaces = 0;
};
namespace gpu_inventory_detail {
struct Counter {
    std::atomic<uint64_t> bytes{0}, surfaces{0};
};
inline std::array<Counter, static_cast<size_t>(GpuSurfaceOwner::Count)> counters;
inline constexpr GUID kInventoryId{0x5a563bc8, 0x60bd, 0x4480, {0xaa, 0xa0, 0x6b, 0x08, 0x68, 0x84, 0x91, 0xee}};
class Lifetime final : public IUnknown {
  public:
    Lifetime(GpuSurfaceOwner owner, uint64_t bytes) : owner_(owner), bytes_(bytes) {
        auto& c = counters[static_cast<size_t>(owner_)];
        c.bytes.fetch_add(bytes_);
        c.surfaces.fetch_add(1);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }

  private:
    ~Lifetime() {
        auto& c = counters[static_cast<size_t>(owner_)];
        c.bytes.fetch_sub(bytes_);
        c.surfaces.fetch_sub(1);
    }
    std::atomic<ULONG> refs_{1};
    GpuSurfaceOwner owner_;
    uint64_t bytes_;
};
} // namespace gpu_inventory_detail
// Logical texel storage for tracked single-mip surfaces across all process adapters.
// Excludes driver alignment, compression, shared imports and untracked external pools.
inline uint64_t LogicalSurfaceBytes(const D3D11_TEXTURE2D_DESC& desc) {
    const uint64_t pixels = uint64_t(desc.Width) * desc.Height * desc.ArraySize * desc.SampleDesc.Count;
    switch (desc.Format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return pixels * 8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_AYUV:
        return pixels * 4;
    case DXGI_FORMAT_NV12:
        return pixels * 3 / 2;
    case DXGI_FORMAT_P010:
        return pixels * 3;
    case DXGI_FORMAT_R8_UNORM:
        return pixels;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return pixels * 2;
    case DXGI_FORMAT_R16G16_UNORM:
        return pixels * 4;
    default:
        return 0;
    }
}
inline GpuSurfaceUsage ReadGpuSurfaceUsage(GpuSurfaceOwner owner) {
    auto& c = gpu_inventory_detail::counters[static_cast<size_t>(owner)];
    return {c.bytes.load(std::memory_order_relaxed), c.surfaces.load(std::memory_order_relaxed)};
}
inline HRESULT CreateTrackedTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                      const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** texture,
                                      GpuSurfaceOwner owner) {
    ScopedPerformanceMeasurement measurement(PerformanceStage::ResourceCreation);
    const HRESULT result = device->CreateTexture2D(desc, data, texture);
    if (SUCCEEDED(result) && texture && *texture) {
        const uint64_t bytes = LogicalSurfaceBytes(*desc);
        if (bytes != 0) {
            auto* lifetime = new (std::nothrow) gpu_inventory_detail::Lifetime(owner, bytes);
            if (lifetime) {
                (*texture)->SetPrivateDataInterface(gpu_inventory_detail::kInventoryId, lifetime);
                lifetime->Release();
            }
        }
    }
    return result;
}
} // namespace exosnap::engine
