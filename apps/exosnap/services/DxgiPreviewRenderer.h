#pragma once

#include "PreviewHelpers.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <recorder_core/recorder_session.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace exosnap {

class DxgiPreviewRenderer {
  public:
    DxgiPreviewRenderer();
    ~DxgiPreviewRenderer();

    DxgiPreviewRenderer(const DxgiPreviewRenderer&) = delete;
    DxgiPreviewRenderer& operator=(const DxgiPreviewRenderer&) = delete;

    bool Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight, uint32_t swapWidth, uint32_t swapHeight);

    // crop_box: optional monitor-relative physical-pixel crop applied during preview.
    // When set, only the specified sub-region of the captured monitor frame is rendered.
    // std::nullopt captures the full monitor (Display and Window targets).
    bool StartCapture(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num, uint32_t frame_rate_den,
                      std::optional<PreviewCropBox> crop_box = std::nullopt);

    void StopCapture();

    void Resize(uint32_t hwndWidth, uint32_t hwndHeight, uint32_t swapWidth, uint32_t swapHeight);

    [[nodiscard]] bool IsActive() const noexcept;
    void GetSourceSize(uint32_t& outWidth, uint32_t& outHeight) const noexcept;

    void Shutdown();

  private:
    static LRESULT CALLBACK ChildWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderThreadProc(recorder_core::CaptureTarget target, uint32_t frame_interval_ms, std::stop_token stop_token);

    bool InitD3D11();
    bool InitSwapChain(uint32_t width, uint32_t height);
    bool InitShaders();
    bool InitCaptureItem(const recorder_core::CaptureTarget& target);
    bool InitFramePool();
    void CleanupCapture();
    void PollAndProcessFrames();
    void RenderFrame();
    void ResizeSwapChainInternal(uint32_t width, uint32_t height);

    HWND parentHwnd_ = nullptr;
    HWND childHwnd_ = nullptr;

    std::jthread renderThread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> initialized_{false};

    std::atomic<bool> resizeRequested_{false};
    std::atomic<uint32_t> requestedWidth_{0};
    std::atomic<uint32_t> requestedHeight_{0};

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState_;

    mutable std::mutex frameMutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latestFrame_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> latestFrameSRV_;
    uint32_t srcWidth_{0};
    uint32_t srcHeight_{0};

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession_{nullptr};
    winrt::event_token closedToken_{};
    std::atomic<bool> sourceLost_{false};

    uint32_t swapWidth_{0};
    uint32_t swapHeight_{0};
    uint32_t initialWidth_{0};
    uint32_t initialHeight_{0};

    // Set in StartCapture before the render thread begins; immutable during rendering.
    // Stores the monitor-relative crop rectangle for Region preview targets.
    std::optional<PreviewCropBox> cropBox_{};
};

} // namespace exosnap
