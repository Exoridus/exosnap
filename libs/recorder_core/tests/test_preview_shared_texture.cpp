#include <gtest/gtest.h>

#include "preview_shared_texture.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

#include <cstdint>
#include <iterator>
#include <vector>

// WARP-backed round-trip tests for the producer-side shared preview texture.
// Follows the WARP pattern established by test_gpu_compositor / the old staging
// ring test: deterministic, no NVENC/real GPU required. Two independent WARP
// devices stand in for the engine device (producer) and the preview device
// (consumer); both resolve to the same software adapter so an NT-handle keyed-
// mutex texture opens across them. These validate (a) the composited pixels
// round-trip byte-exact through the shared surface for both B8G8R8A8 and
// R10G10B10A2, and (b) the non-blocking drop discipline (a publish while the
// consumer holds the keyed mutex fails instead of stalling).

namespace {

using recorder_core::kPreviewSharedConsumerKey;
using recorder_core::kPreviewSharedProducerKey;
using recorder_core::PreviewSharedTexture;

struct D3DTestDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

D3DTestDevice CreateWarpDevice() {
    D3DTestDevice out;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                         out.device.put(), &selected, out.context.put());
    EXPECT_TRUE(SUCCEEDED(hr));
    return out;
}

// Source texture filled with a per-pixel 32-bit pattern (works for any 4-byte
// format: the exact bit interpretation is irrelevant to a byte-exact round trip).
winrt::com_ptr<ID3D11Texture2D> CreatePatternTexture(ID3D11Device* device, uint32_t width, uint32_t height,
                                                     DXGI_FORMAT format, uint32_t seed) {
    std::vector<uint32_t> data(static_cast<size_t>(width) * height);
    for (uint32_t i = 0; i < data.size(); ++i)
        data[i] = seed + i * 2654435761u; // Knuth hash spread so rows differ

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data.data();
    init.SysMemPitch = width * 4u;

    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &init, tex.put())));
    return tex;
}

// Open the shared handle on `consumer`, acquire the keyed mutex, copy the shared
// texture into a CPU-readable staging texture, and return the pixels. Leaves the
// mutex released back to the producer key. Returns true on success.
bool ConsumeSharedPixels(const D3DTestDevice& consumer, HANDLE handle, uint32_t width, uint32_t height,
                         DXGI_FORMAT format, std::vector<uint32_t>& out_pixels) {
    winrt::com_ptr<ID3D11Device1> dev1;
    if (FAILED(consumer.device->QueryInterface(IID_PPV_ARGS(dev1.put()))))
        return false;

    winrt::com_ptr<ID3D11Texture2D> shared;
    if (FAILED(dev1->OpenSharedResource1(handle, IID_PPV_ARGS(shared.put()))) || !shared)
        return false;

    winrt::com_ptr<IDXGIKeyedMutex> mutex;
    if (FAILED(shared->QueryInterface(IID_PPV_ARGS(mutex.put()))))
        return false;

    // Wait up to 1 s (test only) for the producer's "frame ready" release.
    if (mutex->AcquireSync(kPreviewSharedConsumerKey, 1000) != S_OK)
        return false;

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = width;
    sd.Height = height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    winrt::com_ptr<ID3D11Texture2D> staging;
    bool ok = SUCCEEDED(consumer.device->CreateTexture2D(&sd, nullptr, staging.put()));
    if (ok) {
        consumer.context->CopyResource(staging.get(), shared.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(consumer.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            out_pixels.resize(static_cast<size_t>(width) * height);
            const auto* base = static_cast<const uint8_t*>(mapped.pData);
            for (uint32_t y = 0; y < height; ++y) {
                const auto* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * mapped.RowPitch);
                for (uint32_t x = 0; x < width; ++x)
                    out_pixels[static_cast<size_t>(y) * width + x] = row[x];
            }
            consumer.context->Unmap(staging.get(), 0);
        } else {
            ok = false;
        }
    }

    mutex->ReleaseSync(kPreviewSharedProducerKey);
    return ok;
}

void RunRoundTrip(DXGI_FORMAT format) {
    auto producer = CreateWarpDevice();
    auto consumer = CreateWarpDevice();
    ASSERT_TRUE(producer.device);
    ASSERT_TRUE(consumer.device);

    constexpr uint32_t kW = 32, kH = 24;
    PreviewSharedTexture shared;
    HANDLE handle = nullptr;
    std::string err;
    ASSERT_TRUE(shared.Create(producer.device.get(), kW, kH, format, &handle, err)) << err;
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(shared.Valid());
    EXPECT_EQ(shared.Width(), kW);
    EXPECT_EQ(shared.Height(), kH);

    auto src = CreatePatternTexture(producer.device.get(), kW, kH, format, 0xA5A5u);
    ASSERT_TRUE(shared.TryPublish(producer.context.get(), src.get()));
    producer.context->Flush();

    std::vector<uint32_t> pixels;
    ASSERT_TRUE(ConsumeSharedPixels(consumer, handle, kW, kH, format, pixels));

    std::vector<uint32_t> expected(static_cast<size_t>(kW) * kH);
    for (uint32_t i = 0; i < expected.size(); ++i)
        expected[i] = 0xA5A5u + i * 2654435761u;
    ASSERT_EQ(pixels.size(), expected.size());
    EXPECT_EQ(pixels, expected);

    CloseHandle(handle);
}

} // namespace

TEST(PreviewSharedTexture, RoundTripBgra8) {
    RunRoundTrip(DXGI_FORMAT_B8G8R8A8_UNORM);
}

TEST(PreviewSharedTexture, RoundTripR10G10B10A2) {
    RunRoundTrip(DXGI_FORMAT_R10G10B10A2_UNORM);
}

TEST(PreviewSharedTexture, PublishDropsWhileConsumerHoldsMutex) {
    auto producer = CreateWarpDevice();
    auto consumer = CreateWarpDevice();
    ASSERT_TRUE(producer.device);
    ASSERT_TRUE(consumer.device);

    constexpr uint32_t kW = 16, kH = 16;
    PreviewSharedTexture shared;
    HANDLE handle = nullptr;
    std::string err;
    ASSERT_TRUE(shared.Create(producer.device.get(), kW, kH, DXGI_FORMAT_B8G8R8A8_UNORM, &handle, err)) << err;

    auto src = CreatePatternTexture(producer.device.get(), kW, kH, DXGI_FORMAT_B8G8R8A8_UNORM, 1u);

    // First publish succeeds and hands the mutex to the consumer key.
    ASSERT_TRUE(shared.TryPublish(producer.context.get(), src.get()));

    // Consumer opens and acquires the mutex, then holds it.
    winrt::com_ptr<ID3D11Device1> dev1;
    ASSERT_TRUE(SUCCEEDED(consumer.device->QueryInterface(IID_PPV_ARGS(dev1.put()))));
    winrt::com_ptr<ID3D11Texture2D> openedTex;
    ASSERT_TRUE(SUCCEEDED(dev1->OpenSharedResource1(handle, IID_PPV_ARGS(openedTex.put()))));
    winrt::com_ptr<IDXGIKeyedMutex> consumerMutex;
    ASSERT_TRUE(SUCCEEDED(openedTex->QueryInterface(IID_PPV_ARGS(consumerMutex.put()))));
    ASSERT_EQ(consumerMutex->AcquireSync(kPreviewSharedConsumerKey, 1000), S_OK);

    // While the consumer holds the mutex, a producer publish must DROP (return
    // false) rather than block — the encode path is never stalled.
    EXPECT_FALSE(shared.TryPublish(producer.context.get(), src.get()));

    // Once the consumer releases, publishing succeeds again.
    consumerMutex->ReleaseSync(kPreviewSharedProducerKey);
    EXPECT_TRUE(shared.TryPublish(producer.context.get(), src.get()));

    CloseHandle(handle);
}
