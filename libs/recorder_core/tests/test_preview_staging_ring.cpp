#include <gtest/gtest.h>

#include "preview_staging_ring.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstring>
#include <iterator>
#include <stdexcept>
#include <vector>

// WARP-backed D3D11 tests for the two-slot staging ring (Strand 3 slice 1).
// Follows the WARP pattern established by test_gpu_compositor.cpp:
// deterministic, no NVENC/real GPU required. These tests validate the
// *correctness* of the one-tick-behind read pattern (the right content AND
// the matching per-slot timestamp come back at the right time, slot reuse
// doesn't race an outstanding read). Whether Map(DO_NOT_WAIT) actually
// avoids stalls in production is a live-hardware-only concern noted in the
// deliverable -- WARP has no real async GPU pipeline to demonstrate that
// against.

namespace {

using recorder_core::PreviewRingReadGuard;
using recorder_core::PreviewStagingRing;

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

winrt::com_ptr<ID3D11Texture2D> CreateSolidTexture(ID3D11Device* device, int width, int height, uint8_t value) {
    std::vector<uint8_t> data(static_cast<size_t>(width) * height * 4, value);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data.data();
    init.SysMemPitch = static_cast<UINT>(width * 4);

    winrt::com_ptr<ID3D11Texture2D> tex;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &init, tex.put())));
    return tex;
}

D3D11_TEXTURE2D_DESC MakeDesc(int width, int height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    return desc;
}

// TryReadReady uses Map(D3D11_MAP_FLAG_DO_NOT_WAIT) and legitimately reports
// not-ready while the GPU copy is still in flight (observed on WARP too).
// Production simply skips that publish tick and succeeds on a later one;
// this helper mirrors that by retrying with a Flush until the copy lands.
// Bounded so a genuine failure still fails the test instead of hanging.
bool ReadReadyEventually(PreviewStagingRing& ring, ID3D11DeviceContext* context, const uint8_t** out_data,
                         uint32_t* out_row_pitch, uint64_t* out_timestamp_ns) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (ring.TryReadReady(context, out_data, out_row_pitch, out_timestamp_ns))
            return true;
        context->Flush();
        Sleep(1);
    }
    return false;
}

} // namespace

TEST(PreviewStagingRing, NotReadyBeforeTwoSubmits) {
    auto dev = CreateWarpDevice();
    ASSERT_TRUE(dev.device);

    PreviewStagingRing ring;
    ASSERT_TRUE(ring.Initialize(dev.device.get(), MakeDesc(4, 4)));

    auto tex0 = CreateSolidTexture(dev.device.get(), 4, 4, 10);
    const uint8_t* data = nullptr;
    uint32_t pitch = 0;
    uint64_t ts = 0;

    EXPECT_FALSE(ring.TryReadReady(dev.context.get(), &data, &pitch, &ts));

    ring.Submit(dev.context.get(), tex0.get(), 100);
    EXPECT_FALSE(ring.TryReadReady(dev.context.get(), &data, &pitch, &ts));
}

TEST(PreviewStagingRing, ReadsContentAndTimestampSubmittedOneTickBehind) {
    auto dev = CreateWarpDevice();
    ASSERT_TRUE(dev.device);

    PreviewStagingRing ring;
    ASSERT_TRUE(ring.Initialize(dev.device.get(), MakeDesc(4, 4)));

    auto texA = CreateSolidTexture(dev.device.get(), 4, 4, 11);
    auto texB = CreateSolidTexture(dev.device.get(), 4, 4, 22);
    auto texC = CreateSolidTexture(dev.device.get(), 4, 4, 33);

    ring.Submit(dev.context.get(), texA.get(), 1'000); // tick 1: A @ pts 1000
    ring.Submit(dev.context.get(), texB.get(), 2'000); // tick 2: B @ pts 2000

    const uint8_t* data = nullptr;
    uint32_t pitch = 0;
    uint64_t ts = 0;
    ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));
    // One tick behind the most recent submit (B) -> frame A. The timestamp
    // MUST be A's submit timestamp, not the current tick's: stamping the
    // one-tick-old pixels with the newer PTS would skew every downstream
    // pacing decision by one frame.
    EXPECT_EQ(data[0], 11);
    EXPECT_EQ(ts, 1'000u);
    ring.FinishRead(dev.context.get());

    ring.Submit(dev.context.get(), texC.get(), 3'000); // tick 3: C (reuses A's freed slot)

    ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));
    EXPECT_EQ(data[0], 22); // now one tick behind tick 3 -> B
    EXPECT_EQ(ts, 2'000u);
    ring.FinishRead(dev.context.get());
}

TEST(PreviewStagingRing, SecondReadWithoutFinishIsRejected) {
    auto dev = CreateWarpDevice();
    ASSERT_TRUE(dev.device);

    PreviewStagingRing ring;
    ASSERT_TRUE(ring.Initialize(dev.device.get(), MakeDesc(2, 2)));

    auto texA = CreateSolidTexture(dev.device.get(), 2, 2, 1);
    auto texB = CreateSolidTexture(dev.device.get(), 2, 2, 2);
    ring.Submit(dev.context.get(), texA.get(), 1);
    ring.Submit(dev.context.get(), texB.get(), 2);

    const uint8_t* data = nullptr;
    uint32_t pitch = 0;
    uint64_t ts = 0;
    ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));
    EXPECT_FALSE(ring.TryReadReady(dev.context.get(), &data, &pitch, &ts));
    ring.FinishRead(dev.context.get());
}

TEST(PreviewStagingRing, SubmitWhileReadPendingIsIgnored) {
    auto dev = CreateWarpDevice();
    ASSERT_TRUE(dev.device);

    PreviewStagingRing ring;
    ASSERT_TRUE(ring.Initialize(dev.device.get(), MakeDesc(2, 2)));

    auto texA = CreateSolidTexture(dev.device.get(), 2, 2, 5);
    auto texB = CreateSolidTexture(dev.device.get(), 2, 2, 6);
    auto texRogue = CreateSolidTexture(dev.device.get(), 2, 2, 99);

    ring.Submit(dev.context.get(), texA.get(), 10);
    ring.Submit(dev.context.get(), texB.get(), 20);

    const uint8_t* data = nullptr;
    uint32_t pitch = 0;
    uint64_t ts = 0;
    ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));

    // Contract violation: Submit() while a read is outstanding. Must be a
    // no-op rather than corrupting the mapped buffer out from under the
    // caller.
    ring.Submit(dev.context.get(), texRogue.get(), 999);
    EXPECT_EQ(data[0], 5);
    EXPECT_EQ(ts, 10u);

    ring.FinishRead(dev.context.get());
}

TEST(PreviewStagingRing, ResetRequiresTwoFreshSubmitsAgain) {
    auto dev = CreateWarpDevice();
    ASSERT_TRUE(dev.device);

    PreviewStagingRing ring;
    ASSERT_TRUE(ring.Initialize(dev.device.get(), MakeDesc(2, 2)));

    auto texA = CreateSolidTexture(dev.device.get(), 2, 2, 7);
    auto texB = CreateSolidTexture(dev.device.get(), 2, 2, 8);
    ring.Submit(dev.context.get(), texA.get(), 1);
    ring.Submit(dev.context.get(), texB.get(), 2);

    ring.Reset();

    const uint8_t* data = nullptr;
    uint32_t pitch = 0;
    uint64_t ts = 0;
    EXPECT_FALSE(ring.TryReadReady(dev.context.get(), &data, &pitch, &ts));

    ring.Submit(dev.context.get(), texA.get(), 3);
    EXPECT_FALSE(ring.TryReadReady(dev.context.get(), &data, &pitch, &ts));
    ring.Submit(dev.context.get(), texB.get(), 4);
    ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));
    EXPECT_EQ(ts, 3u);
    ring.FinishRead(dev.context.get());
}

TEST(PreviewStagingRing, ReadGuardUnmapsOnException) {
    auto dev = CreateWarpDevice();
    ASSERT_TRUE(dev.device);

    PreviewStagingRing ring;
    ASSERT_TRUE(ring.Initialize(dev.device.get(), MakeDesc(2, 2)));

    auto texA = CreateSolidTexture(dev.device.get(), 2, 2, 1);
    auto texB = CreateSolidTexture(dev.device.get(), 2, 2, 2);
    ring.Submit(dev.context.get(), texA.get(), 1);
    ring.Submit(dev.context.get(), texB.get(), 2);

    const uint8_t* data = nullptr;
    uint32_t pitch = 0;
    uint64_t ts = 0;

    // Simulate an exception thrown between TryReadReady and FinishRead
    // (bad_alloc while sizing the BGRA buffer, or a throwing app callback):
    // the guard must unmap so the NEXT tick still works.
    try {
        ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));
        PreviewRingReadGuard guard(ring, dev.context.get());
        throw std::runtime_error("simulated callback failure");
    } catch (const std::runtime_error&) {
    }

    // Ring must be fully usable again: submit + read succeeds.
    auto texC = CreateSolidTexture(dev.device.get(), 2, 2, 3);
    ring.Submit(dev.context.get(), texC.get(), 3);
    ASSERT_TRUE(ReadReadyEventually(ring, dev.context.get(), &data, &pitch, &ts));
    EXPECT_EQ(data[0], 2);
    EXPECT_EQ(ts, 2u);
    ring.FinishRead(dev.context.get());
}
