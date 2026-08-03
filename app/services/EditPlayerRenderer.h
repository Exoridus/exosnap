// app/services/EditPlayerRenderer.h
#pragma once

#include <recorder_core/edit_frame_gpu_converter.h>
#include <recorder_core/edit_player_engine.h>

#include <cstdint>
#include <memory>

#include <d3d11.h>
#include <dxgi1_2.h> // IDXGISwapChain1
#include <windows.h>
#include <wrl/client.h>

namespace exosnap {

// GPU render path for the Edit page's video player, hosted by a native child
// HWND created by EditPlayerSurface -- same relationship DxgiPreviewRenderer
// has with PreviewSurface, deliberately much smaller (no capture graph, no
// webcam PiP, no cursor sprite, no snapshot path).
class EditPlayerRenderer {
  public:
    EditPlayerRenderer();
    ~EditPlayerRenderer();
    EditPlayerRenderer(const EditPlayerRenderer&) = delete;
    EditPlayerRenderer& operator=(const EditPlayerRenderer&) = delete;

    bool Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight);
    void Resize(uint32_t hwndWidth, uint32_t hwndHeight);
    // Presents one decoded frame. Thread-safety/threading model: Task 4 decides
    // and documents here (own render thread vs. caller's thread) as part of its
    // implementation -- not fixed by this stub.
    void PresentFrame(recorder_core::RawDecodedVideoFrame frame, float hdr_peak_scale);
    void ShowPlaceholder(const std::wstring& text);
    void Shutdown();

  private:
    HWND parentHwnd_ = nullptr;
    HWND childHwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    std::unique_ptr<recorder_core::EditFrameGpuConverter> converter_;
};

} // namespace exosnap
