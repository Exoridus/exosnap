// app/services/EditPlayerRenderer.h
#pragma once

#include <recorder_core/edit_frame_gpu_converter.h>
#include <recorder_core/edit_player_engine.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <d3d11.h>
#include <dxgi1_2.h> // IDXGISwapChain1
#include <windows.h>
#include <wrl/client.h>

namespace exosnap {

// GPU render path for the Edit page's video player, hosted by a native child
// HWND created by EditPlayerSurface -- same relationship DxgiPreviewRenderer
// has with PreviewSurface, deliberately much smaller (no capture graph, no
// webcam PiP, no cursor sprite, no snapshot path).
//
// Threading model (Task 4 decision): a DEDICATED render thread, matching
// DxgiPreviewRenderer's ownership model -- Initialize() launches it and
// blocks until the render thread has brought up the D3D11 device/swap chain
// (so Initialize()'s return value is an honest, immediate answer, the same
// contract the Task 1 stub already promised); Shutdown()/the destructor stop
// and join it. PresentFrame()/ShowPlaceholder()/Resize() are cheap,
// thread-safe hand-offs into a single-slot "next thing to show" mailbox
// (guarded by stateMutex_/stateCv_) -- they never touch the GPU and never
// block the caller. The render thread wakes on any hand-off, keeps only the
// NEWEST one (an older undrawn frame is superseded, never queued -- this
// class always shows the freshest state, replacing the old 33ms UI-timer /
// PollFrame() poll the spec's "Presentation cadence" section describes),
// reads the clock snapshot set via SetClockUs() to decide whether a pending
// frame is still worth drawing (at/before the clock: draw; already passed:
// dropped before any GPU work touches it), and only then uploads/converts/
// draws/Present()s.
//
// A dedicated thread (rather than doing this work on the caller's thread
// inside PresentFrame itself) was chosen deliberately: PresentFrame is called
// from the engine's own decode thread (continuous playback) or its seek
// thread (scrub), and the engine's own contract allows a slow video callback
// to block that thread as backpressure (see EditPlayerEngine::
// StartPlaybackDecode's doc comment). Doing GPU work -- texture upload, a
// shader pass, and a vsync-locked Present() -- directly on that thread would
// tie decode pacing to the DISPLAY's refresh rate instead of to decode
// throughput, and would make a slow scrub-seek draw block the seek worker's
// next DecodeFrameAtRaw call. A dedicated render thread isolates GPU/vsync
// latency from every caller, exactly the property DxgiPreviewRenderer's own
// dedicated thread already provides for the Record page's live preview.
class EditPlayerRenderer {
  public:
    EditPlayerRenderer();
    ~EditPlayerRenderer();
    EditPlayerRenderer(const EditPlayerRenderer&) = delete;
    EditPlayerRenderer& operator=(const EditPlayerRenderer&) = delete;

    // Creates the native child HWND under parentHwnd and starts the render
    // thread, blocking until it has finished bringing up the D3D11 device,
    // swap chain, shared shaders and EditFrameGpuConverter -- so the return
    // value is an immediate, honest answer (false on any failure: no hardware
    // D3D11 adapter, swap-chain creation failure, etc.), matching the Task 1
    // stub's "honestly reports not-ready rather than faking success"
    // contract. hwndWidth/hwndHeight are physical (DPI-scaled) pixels, same
    // convention as DxgiPreviewRenderer::Initialize.
    bool Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight);
    // Resizes the child HWND (immediately, on the caller's thread -- a plain
    // SetWindowPos) and asks the render thread to resize the swap chain on
    // its next wake. hwndWidth/hwndHeight are physical pixels.
    void Resize(uint32_t hwndWidth, uint32_t hwndHeight);
    // Presents one decoded frame -- see the threading-model comment above.
    // Thread-safe; callable from any thread, including the engine's own
    // decode/seek threads. A no-op before Initialize() has succeeded.
    void PresentFrame(recorder_core::RawDecodedVideoFrame frame, float hdr_peak_scale);
    // Shows `text` as a centered caption over the panel background instead of
    // whatever was last presented (an empty string shows the plain panel
    // background). Supersedes any not-yet-drawn PresentFrame() call, mirroring
    // EditPlayerSurface::clearFrame()'s old "drop the frame, show the
    // placeholder" contract. Thread-safe; a no-op before Initialize().
    void ShowPlaceholder(const std::wstring& text);
    // Updates the playback-clock snapshot PresentFrame's present-gate
    // consults (see the class comment): absolute media time in microseconds,
    // or a negative value when no clock is available (nothing is dropped in
    // that case). A plain atomic store -- not a callback -- deliberately:
    // the render thread must never call back into caller-owned state (e.g.
    // EditPlayerSession, which lives on a different object with its own
    // lifetime), only ever read state this object itself owns, so its
    // Shutdown()/destructor fully answers "can anything of mine still be
    // read". Callers refresh this once per presentation tick from the SAME
    // atomic snapshot EditPlayerEngine's own decode thread already reads
    // (see EditPlayerSession::ClockSnapshotUs) -- do not build a second
    // clock. Thread-safe; callable from any thread, at any time, including
    // before Initialize().
    void SetClockUs(int64_t media_time_us) noexcept;
    // True once a real decoded frame (not the placeholder) has actually been
    // drawn -- mirrors DxgiPreviewRenderer::HasPresentedFrame()'s shape.
    [[nodiscard]] bool HasPresentedFrame() const noexcept;
    // Toggles the OS-level visibility of the native child window without
    // touching the render thread's lifecycle -- mirrors DxgiPreviewRenderer::
    // SetChildWindowVisible exactly (same bug class: Qt's native-window hide
    // cascade is not guaranteed to reach a manually created WS_CHILD window
    // in this app's frameless/custom-chrome window in the same paint cycle
    // as the next page's first paint). Never blocks; safe at any time,
    // including before Initialize() (a no-op then).
    void SetChildWindowVisible(bool visible) noexcept;
    void Shutdown();

    // Test-only: the native child HWND, so widget tests can assert its real
    // Win32 geometry after a resize. Neither PreviewSurface nor
    // DxgiPreviewRenderer expose an equivalent (their tests don't need
    // window-geometry assertions), but this class's resize test does.
    [[nodiscard]] HWND ChildHwndForTest() const noexcept {
        return childHwnd_;
    }

  private:
    void RenderThreadProc(uint32_t initWidth, uint32_t initHeight, std::promise<bool> initPromise,
                          std::stop_token stop_token);
    bool InitD3D11();
    bool InitSwapChain(uint32_t width, uint32_t height);
    bool InitShaders();
    void ResizeSwapChainInternal(uint32_t width, uint32_t height);
    // (Re)creates the intermediate BGRA8 render target EditFrameGpuConverter
    // writes into, only when the frame's own dimensions change.
    bool EnsureConvertedTarget(uint32_t width, uint32_t height);
    void UploadAndConvert(const recorder_core::RawDecodedVideoFrame& frame, float hdr_peak_scale);
    // GDI-renders `text` centered over the panel background into an
    // off-screen bitmap sized to the swap chain's current dimensions, then
    // uploads it as the placeholder SRV -- the same class of "sprite"
    // technique DxgiPreviewRenderer's OSD sprites use, simplified: this
    // sprite always covers the whole client area, so no separate contain-fit
    // or alpha blend is needed (drawn through the shared shader's opaque
    // mode, same as a video frame blit).
    void RegeneratePlaceholderTexture(const std::wstring& text);
    void DrawAndPresent();
    // Draws `srv` as an opaque textured quad into the sub-rect (x, y, w, h)
    // of the current render target (viewport-scoped full-screen-triangle,
    // the same technique DxgiPreviewRenderer's OSD sprites/frame blit use).
    void DrawSprite(ID3D11ShaderResourceView* srv, long x, long y, long w, long h);
    void ReleaseGpuResources();

    HWND parentHwnd_ = nullptr;
    HWND childHwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    std::unique_ptr<recorder_core::EditFrameGpuConverter> converter_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerState_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

    // Intermediate target EditFrameGpuConverter::Convert writes into (exactly
    // frame.width x frame.height, per its contract); contain-fit-blitted into
    // the swap chain's back buffer by DrawAndPresent.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> convertedTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> convertedSRV_;
    uint32_t convertedW_ = 0;
    uint32_t convertedH_ = 0;
    // The most recently converted frame's own dimensions (used to contain-fit
    // convertedSRV_ into the back buffer -- may differ from convertedW_/H_
    // only transiently, while a resize to a smaller cache is still pending).
    uint32_t frameSrcW_ = 0;
    uint32_t frameSrcH_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> placeholderTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> placeholderSRV_;
    uint32_t placeholderTexW_ = 0;
    uint32_t placeholderTexH_ = 0;
    std::wstring lastPlaceholderText_;

    // Render-thread-owned display state: true once a real frame has been
    // uploaded and should be shown instead of the placeholder. Reset by a
    // ShowPlaceholder() hand-off.
    bool showFrame_ = false;

    std::jthread renderThread_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> hasPresentedFrame_{false};

    // --- Single-slot mailbox: caller threads write, the render thread reads ---
    std::mutex stateMutex_;
    std::condition_variable stateCv_;
    std::optional<recorder_core::RawDecodedVideoFrame> pendingFrame_;
    float pendingPeakScale_ = 1.0f;
    bool pendingPlaceholderDirty_ = false;
    std::wstring pendingPlaceholderText_;
    bool resizePending_ = false;
    uint32_t pendingSwapWidth_ = 0;
    uint32_t pendingSwapHeight_ = 0;

    // Independent of stateMutex_ on purpose: updated far more often (every
    // presentation tick, ~60-240 Hz) than the mailbox above, and a plain
    // atomic needs no lock at all -- see SetClockUs's doc comment.
    std::atomic<int64_t> clockUs_{-1};
};

} // namespace exosnap
