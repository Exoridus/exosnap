// Prep-task (Task 1) smoke test for EditFrameGpuConverter: proves Init()
// succeeds against a real D3D11 device/context. Convert() is a fixed-color
// debug stub at this point in the plan (Task 2 replaces the whole file with
// the real shader conversion and is expected to extend this test then).

#include <gtest/gtest.h>

#include <recorder_core/edit_frame_gpu_converter.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <iterator>

namespace {

using recorder_core::EditFrameGpuConverter;

struct D3DTestDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

// Same WARP-device construction as test_gpu_hdr_tonemap.cpp -- no real GPU
// needed, deterministic in CI.
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

TEST(EditFrameGpuConverterTest, InitSucceedsAgainstRealDevice) {
    D3DTestDevice warp = CreateWarpDevice();
    ASSERT_NE(warp.device, nullptr);
    ASSERT_NE(warp.context, nullptr);

    EditFrameGpuConverter converter;
    std::string err;
    EXPECT_TRUE(converter.Init(warp.device.get(), warp.context.get(), err)) << err;
}

} // namespace
