#include <gtest/gtest.h>

#include "services/DxgiPreviewRenderer.h"

#include <recorder_core/preview_shared_texture.h>
#include <recorder_core/preview_tap.h>
#include <recorder_core/recorder_session.h>

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <thread>

// Regression test for the webcam-PiP handle/box desync bug: PreviewSurface's
// hit-test rect (displayedFrameRect(), used for both mouse hit-testing and the
// normalized placement pushed to the DXGI overlay) contain-fits against
// DxgiPreviewRenderer::GetSourceSize(). Before the fix, GetSourceSize() only
// ever reported the preview's own WGC-capture dimensions (srcWidth_/srcHeight_),
// which stop being updated (PushedSourceState::PollsWgc() == false) the instant
// a pushed source becomes active -- so on a fresh pushed source (recording
// start, capture-target switch, idle-hub handoff) GetSourceSize() kept
// reporting a stale/unrelated size forever, while RenderFrame() itself
// correctly contain-fit the on-screen PiP against the LIVE
// pushedWidth_/pushedHeight_. Any aspect-ratio difference between the two
// decoupled the drag/resize handle box from the actually-rendered PiP.
//
// This is a live-hardware test: DxgiPreviewRenderer::InitD3D11() hardcodes
// D3D_DRIVER_TYPE_HARDWARE (it is production preview-rendering code, not a
// WARP-testable pure helper), so the producer device here must resolve to the
// same real adapter to open the renderer's shared handle. See LABELS live in
// CMakeLists.txt; GTEST_SKIP covers GPU-less/headless runners.

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kTestWindowClass = L"ExoSnapDxgiPreviewPushedSourceTestWnd";

HWND CreateHiddenTestWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTestWindowClass;
    // Registering twice (across test cases in one process) is fine; ignore the
    // "already exists" failure the same way production Initialize() does.
    RegisterClassExW(&wc);

    return CreateWindowExW(0, kTestWindowClass, L"", WS_POPUP, 0, 0, 640, 480, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

bool CreateHardwareDevice(ComPtr<ID3D11Device>& out_device, ComPtr<ID3D11DeviceContext>& out_context) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                         out_device.GetAddressOf(), nullptr, out_context.GetAddressOf());
    return SUCCEEDED(hr) && out_device;
}

ComPtr<ID3D11Texture2D> MakeSourceTexture(ID3D11Device* device, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> tex;
    device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    return tex;
}

} // namespace

class DxgiPreviewPushedSourceSizeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        if (!CreateHardwareDevice(producer_device_, producer_context_)) {
            GTEST_SKIP() << "No hardware D3D11 adapter in this environment (headless CI runner).";
        }

        hwnd_ = CreateHiddenTestWindow();
        ASSERT_NE(hwnd_, nullptr);
        ASSERT_TRUE(renderer_.Initialize(hwnd_, 640, 480, 640, 480));

        recorder_core::CaptureTarget target{};
        target.kind = recorder_core::CaptureTarget::Kind::Monitor;
        target.native_id = reinterpret_cast<uintptr_t>(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY));
        ASSERT_TRUE(renderer_.StartPushedOnly(target, 60, 1));
    }

    void TearDown() override {
        renderer_.StopCapture();
        renderer_.Shutdown();
        if (hwnd_ != nullptr)
            DestroyWindow(hwnd_);
    }

    // Repeatedly publishes into `shared` and polls GetSourceSize() until it
    // reports (w, h) or the timeout elapses. Repeated publishing is required:
    // BeginPushedSource only hands over the handle, and the render thread's
    // ConsumePushedFrame() needs at least one successful producer publish
    // before the non-blocking keyed-mutex handshake lets it copy a frame.
    bool PublishUntilSourceSize(recorder_core::PreviewSharedTexture& shared, ID3D11Texture2D* src, uint32_t w,
                                uint32_t h, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        uint32_t got_w = 0;
        uint32_t got_h = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            shared.TryPublish(producer_context_.Get(), src);
            renderer_.GetSourceSize(got_w, got_h);
            if (got_w == w && got_h == h)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    HWND hwnd_ = nullptr;
    exosnap::DxgiPreviewRenderer renderer_;
    ComPtr<ID3D11Device> producer_device_;
    ComPtr<ID3D11DeviceContext> producer_context_;
};

TEST_F(DxgiPreviewPushedSourceSizeTest, SourceSizeTracksNewlyPushedFrameDimensions) {
    constexpr uint32_t kW1 = 640;
    constexpr uint32_t kH1 = 480; // 4:3
    constexpr uint32_t kW2 = 1280;
    constexpr uint32_t kH2 = 480; // deliberately a DIFFERENT aspect ratio than kW1 x kH1

    recorder_core::PreviewSharedTexture shared1;
    HANDLE handle1 = nullptr;
    std::string err;
    ASSERT_TRUE(shared1.Create(producer_device_.Get(), kW1, kH1, DXGI_FORMAT_B8G8R8A8_UNORM, &handle1, err)) << err;
    ComPtr<ID3D11Texture2D> src1 = MakeSourceTexture(producer_device_.Get(), kW1, kH1);
    ASSERT_TRUE(src1);

    recorder_core::PreviewTapDesc tap{};
    renderer_.BeginPushedSource(handle1, kW1, kH1, tap, /*raw_source_frames=*/true);
    ASSERT_TRUE(PublishUntilSourceSize(shared1, src1.Get(), kW1, kH1, std::chrono::seconds(3)))
        << "GetSourceSize() never converged to the first pushed source's real dimensions";

    // Hand the renderer a SECOND pushed source at a different aspect ratio -- the
    // exact capture-target-switch / recording-start scenario that decoupled the
    // webcam PiP handle box from the actually-rendered PiP before this fix.
    recorder_core::PreviewSharedTexture shared2;
    HANDLE handle2 = nullptr;
    ASSERT_TRUE(shared2.Create(producer_device_.Get(), kW2, kH2, DXGI_FORMAT_B8G8R8A8_UNORM, &handle2, err)) << err;
    ComPtr<ID3D11Texture2D> src2 = MakeSourceTexture(producer_device_.Get(), kW2, kH2);
    ASSERT_TRUE(src2);

    renderer_.BeginPushedSource(handle2, kW2, kH2, tap, /*raw_source_frames=*/true);
    ASSERT_TRUE(PublishUntilSourceSize(shared2, src2.Get(), kW2, kH2, std::chrono::seconds(3)))
        << "GetSourceSize() never converged to the second pushed source's real dimensions -- "
           "PreviewSurface::displayedFrameRect() would keep contain-fitting against the FIRST "
           "source's aspect ratio while RenderFrame() composites the PiP against the new one, "
           "decoupling the drag/resize handle box from the actual rendered webcam.";
}
