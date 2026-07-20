#pragma once

#include "PreviewHelpers.h"
#include "PushedSourceState.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <recorder_core/cursor_sprite.h>
#include <recorder_core/gpu_hdr_tonemap.h>
#include <recorder_core/overlay_shader.h>
#include <recorder_core/preview_tap.h>
#include <recorder_core/recorder_session.h>

#include <d3d11.h>
#include <d3d11_1.h>
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

    // Start the render thread with NO capture of its own: the preview is fed
    // exclusively through BeginPushedSource (a DXGI-hub producer, or the engine
    // during recording). The thread brings up D3D, the swap chain and the
    // shaders, then presents pushed frames; WGC is never initialised, and
    // EndPushedSource drops the source instead of rebuilding a WGC graph — the
    // feeder decides what is pushed next. `target` names the monitor the frames
    // will show (cursor-sprite bounds only); nothing is captured from it here.
    bool StartPushedOnly(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num, uint32_t frame_rate_den);

    void StopCapture();

    void Resize(uint32_t hwndWidth, uint32_t hwndHeight, uint32_t swapWidth, uint32_t swapHeight);

    [[nodiscard]] bool IsActive() const noexcept;
    // Dimensions of whatever the render thread is CURRENTLY drawing as the frame
    // background (the preview's own WGC capture, or a pushed source once its
    // first frame has been consumed) — never a stale value from a source that is
    // no longer being drawn. PreviewSurface::displayedFrameRect() contain-fits
    // against this, so it must always agree with the content rect RenderFrame()
    // itself computes, or the webcam PiP hit-test/handle box decouples from the
    // actually-rendered PiP. Thread-safe (frameMutex_-guarded).
    void GetSourceSize(uint32_t& outWidth, uint32_t& outHeight) const noexcept;

    // Toggle the OS-level visibility of the native child window without touching
    // capture/render-thread lifecycle (unlike StopCapture()/Shutdown(), this never
    // blocks: it is a single ShowWindow call). Used to hide the DXGI child HWND
    // the instant the owning PreviewSurface is hidden (e.g. navigating away from
    // the Record page) so it cannot linger composited over whatever page is shown
    // next for even one frame, instead of relying on Qt's native-window hide
    // cascade — which, in a custom-chrome/frameless window, is not guaranteed to
    // reach this manually created WS_CHILD window in the same paint cycle as the
    // new page's first paint. Safe to call at any time, including before
    // Initialize() (no-op) and while the render thread is running.
    void SetChildWindowVisible(bool visible) noexcept;

    // --- Webcam PiP overlay (composited into the preview swapchain) ---
    // The native child HWND occludes Qt painting, so the live PiP (and its edit
    // chrome) is drawn here for true WYSIWYG. Placement is normalized to the same
    // content rectangle the recording compositor uses, so preview and output match.
    // Mirror, opacity and the chroma key are handed to the same shader the recording
    // compositor runs (recorder_core/overlay_shader.h), so the PiP the user sets up
    // here is pixel-for-pixel the PiP the encoder writes.
    // Thread-safe: called from the UI thread; applied on the render thread.
    void SetWebcamOverlayState(bool enabled, bool selected, float nx, float ny, float nw, float nh, bool mirror,
                               float opacity, const recorder_core::ChromaKeyParams& chroma);
    // bgra: tightly indexable BGRA pixels (stride bytes per row). nullptr clears it.
    void SetWebcamOverlayFrame(const uint8_t* bgra, int width, int height, int stride);

    // --- On-screen-display sprites (preview-only chrome above everything) ---
    // The Record preview's meta/stats text rows are Qt child widgets, which the
    // native child HWND occludes; the surface rasterizes them and hands them here
    // to be composited LAST — above the frame, the webcam PiP and the cursor —
    // so the footer-above-PiP z-order matches the Qt paint path. Never part of
    // the snapshot readback (that re-blits the raw source), so OSD chrome never
    // leaks into screenshots or "what the encoder sees".
    // slot: 0..kOsdSpriteSlots-1. bgra: straight-alpha BGRA (stride bytes/row);
    // nullptr clears the slot. destX/destY: top-left in swap-chain pixels.
    // Thread-safe: called from the UI thread; drawn on the render thread.
    // Slot 0/1: preview meta/stats text rows (PreviewSurface::syncOsdToDxgi).
    static constexpr int kOsdSpriteSlots = 2;
    void SetOsdSprite(int slot, const uint8_t* bgra, int width, int height, int stride, int destX, int destY);

    // --- Webcam-magnifier dim scrim (composited BELOW the webcam PiP) ---
    // Unlike OSD sprites, which are composited LAST (above everything, including
    // the webcam PiP), this single scrim draws BETWEEN the base frame and the
    // webcam-overlay layer -- so it can dim the background behind an enlarged or
    // animating webcam PiP without darkening the PiP itself, matching the Qt
    // paint path's z-order (scrim fillRect first, enlarged PiP drawImage on top).
    // bgra: straight-alpha BGRA (stride bytes/row); nullptr, or a non-positive
    // width or height, clears it. destX/destY: top-left in swap-chain pixels.
    // Used exclusively by PreviewSurface::syncEnlargedWebcamToDxgi() for the
    // magnifier's dim scrim during its enlarge/collapse animation.
    // Thread-safe: called from the UI thread; drawn on the render thread.
    void SetWebcamDimScrim(const uint8_t* bgra, int width, int height, int stride, int destX, int destY);

    void Shutdown();

    // --- Pushed source mode (WYSIWYG preview during recording) ---
    // During recording the engine shares its composited, pre-encode frame via an
    // NT-handle keyed-mutex texture. BeginPushedSource switches the preview to
    // sample that shared texture instead of running its own WGC capture: the
    // render thread opens the handle, STOPS the WGC capture graph (no second
    // capture), and renders the engine's frames (which already contain the webcam
    // PiP, so the renderer's own overlay is suppressed to avoid a double draw).
    //
    // nt_handle: the shared NT handle; ownership transfers to the renderer, which
    // opens it via OpenSharedResource1 on its render thread and CloseHandle's it.
    // tap: the display transform the shared surface needs before drawing
    // (recorder_core/preview_tap.h). A native HDR10 session shares linear scRGB
    // FP16; the render thread tone-maps it to SDR with its own HdrToneMapper.
    // raw_source_frames: true when the frames are RAW captures (an idle DXGI-hub
    // source) carrying neither cursor nor webcam PiP — the renderer then draws
    // both itself; false for the engine's recording frames (overlays baked in).
    // Thread-safe: may be called from any thread; only stashes the handle + signals
    // the render thread (no D3D on the caller's thread). Until the first shared
    // frame arrives the preview holds its last presented image (no black flash).
    void BeginPushedSource(void* nt_handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap,
                           bool raw_source_frames = false);
    // Revert to the normal WGC preview path. Signals the render thread to release the
    // engine's shared resources and rebuild its OWN WGC capture graph IN PLACE — the
    // D3D device, swap chain and shaders stay alive, so there is no teardown and no
    // black flash (BeginPushedSource's exact inverse). Also drains any handle that was
    // signalled but never opened. May be called from any thread; safe to call when
    // not in pushed mode (a no-op there). The revert itself runs on the render thread.
    void EndPushedSource();

    // --- Snapshot (screenshot-button) support ---
    // One-shot readback of the next rendered frame: whatever is currently on
    // screen — the fully composited, tone-mapped, WYSIWYG content (same source
    // RenderFrame() presents, including the webcam PiP and cursor) — as tightly
    // packed BGRA8. Thread-safe: callable from any thread. The callback fires on
    // the RENDER thread (not marshaled here — mirrors how the engine's own
    // RequestFrameSnapshot callback fires from VideoThread; callers that touch
    // Qt/UI state must hop to the main thread themselves, as RecordingCoordinator
    // already does for the engine's snapshot path).
    using SnapshotCallback =
        std::function<void(bool ok, uint32_t width, uint32_t height, std::vector<uint8_t> bgra, std::string error)>;
    void RequestSnapshot(SnapshotCallback callback);

    // Fires once, on the RENDER thread, the first time a real source frame is
    // presented (same condition under which RequestSnapshot succeeds). Set it
    // before StartCapture/StartPushedOnly. Callers marshal to their own thread.
    void SetFirstFramePresentedCallback(std::function<void()> cb);
    // True once a real source frame has been presented in this renderer run.
    [[nodiscard]] bool HasPresentedFrame() const noexcept;

  private:
    static LRESULT CALLBACK ChildWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RenderThreadProc(const recorder_core::CaptureTarget& target, uint32_t frame_interval_ms,
                          std::stop_token stop_token);

    bool InitD3D11();
    bool InitSwapChain(uint32_t width, uint32_t height);
    bool InitShaders();
    bool InitCaptureItem(const recorder_core::CaptureTarget& target);
    bool InitFramePool();
    void CleanupCapture();
    // Close only the WGC capture graph (session/frame pool/item) while keeping the
    // D3D device, swap chain and shaders alive — used when entering pushed mode so
    // the preview's own capture truly stops. Render-thread only.
    void StopCaptureGraph();
    // Adopt a pending pushed-source handle: open it on the render device, allocate
    // the private copy target, and activate pushed rendering. Render-thread only.
    void AdoptPendingPushedSource();
    // Return the preview to its own WGC capture after recording stops: release the
    // engine's shared resources and rebuild the WGC capture graph in place (no
    // device/swap-chain teardown). The inverse of AdoptPendingPushedSource +
    // StopCaptureGraph. Render-thread only.
    void RevertToWgcCapture(const recorder_core::CaptureTarget& target);
    // Non-blocking: acquire the shared keyed mutex, copy the latest engine frame
    // into the private local texture, release. Render-thread only.
    void ConsumePushedFrame();
    void ReleasePushedResources();
    void PollAndProcessFrames();
    void RenderFrame();
    void ResizeSwapChainInternal(uint32_t width, uint32_t height);
    // Render-thread helpers for the PiP overlay. Must hold overlayMutex_.
    void UploadOverlayTexture();
    void EnsureChromeTexture();
    // Draw the PiP video + (optional) edit chrome into the content rectangle.
    void RenderWebcamOverlay(int contentX, int contentY, int contentW, int contentH);
    // Draw the OSD sprites (meta/stats rows) last, above frame + PiP + cursor.
    // Render-thread only; takes overlayMutex_ itself.
    void RenderOsdSprites();
    // Draw the webcam-magnifier dim scrim between the base frame and the webcam
    // PiP (so the scrim dims the background, not the enlarged PiP). No-ops cheaply
    // when the scrim is cleared. Render-thread only; takes overlayMutex_ itself.
    void RenderWebcamDimScrim();
    // Draw the live mouse cursor over a RAW pushed background (an idle DXGI-hub
    // frame — Output Duplication composites no cursor). Queries the Win32 cursor,
    // maps it from the captured monitor into the content rectangle with the same
    // shared arithmetic the recording compositor uses, and draws it through the
    // shared overlay shader (OverlayMode::Cursor). Render-thread only; a no-op
    // without valid monitor bounds.
    void RenderCursorSprite(int contentX, int contentY, int contentW, int contentH);
    // Services a pending RequestSnapshot() by blitting the current source SRV
    // (the SAME one RenderFrame() draws to screen this tick — pushedSdrSRV_/
    // pushedLocalSRV_/latestFrameSRV_) into a dedicated full-resolution render
    // target and reading THAT back, instead of the small, letterboxed preview
    // swap-chain back buffer: the preview widget is typically far smaller than
    // the captured source and pillarboxed/letterboxed with black bars, neither
    // of which belongs in "what the encoder sees". srv/srcW/srcH being null/0
    // means no real source has been drawn yet this tick (e.g. the first ticks
    // after the preview starts) — the callback then reports failure instead of
    // handing back a black image. Render-thread only; called from RenderFrame()
    // right before Present().
    void PerformSnapshotIfRequested(ID3D11ShaderResourceView* srv, uint32_t srcW, uint32_t srcH);

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
    // Constant-colour blend state for the webcam PiP quad: SrcBlend/DestBlend read the
    // BLEND_FACTOR passed to OMSetBlendState, so a single state serves any opacity.
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlayBlendState_;
    // Feeds the shared overlay shader's b0 slot; rewritten per draw.
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;

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

    // True for a StartPushedOnly thread: no WGC graph exists at any point, and a
    // pushed-source end drops the source instead of rebuilding one. Set before
    // thread creation (same fence as cropBox_); immutable during rendering.
    bool pushedOnlyMode_ = false;

    // --- Cursor sprite over raw pushed frames (render-thread owned) ---
    // The captured monitor's rectangle in virtual-screen coordinates, resolved in
    // StartCapture for Monitor targets (immutable during rendering, like cropBox_).
    // Maps GetCursorInfo's screen position into source-frame pixels.
    RECT cursorSpriteBounds_{};
    bool cursorSpriteBoundsValid_ = false;
    // Cursor bitmap cache, keyed by (HCURSOR, clip) — recreated only when the
    // cursor image or its edge crop changes, not per frame.
    HCURSOR cursorSpriteHandle_ = nullptr;
    recorder_core::Win32CursorBitmap cursorSpriteBitmap_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursorSpriteTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorSpriteSRV_;
    recorder_core::CursorSpriteClip cursorSpriteTexClip_{};

    // --- Webcam PiP overlay state (guarded by overlayMutex_) ---
    mutable std::mutex overlayMutex_;
    bool overlayEnabled_ = false;
    bool overlaySelected_ = false;
    bool overlayMirror_ = false;
    float overlayOpacity_ = 1.0f;
    recorder_core::ChromaKeyParams overlayChroma_{};
    float overlayNx_ = 0.0f;
    float overlayNy_ = 0.0f;
    float overlayNw_ = 0.25f;
    float overlayNh_ = 0.25f;
    // Latest webcam frame, tightly packed BGRA. Always unmirrored: the shader flips
    // the texcoord, so a mirror toggle costs no re-upload.
    std::vector<uint8_t> overlayBgra_;
    int overlayW_ = 0;
    int overlayH_ = 0;
    bool overlayDirty_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overlayTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlaySRV_;
    int overlayTexW_ = 0;
    int overlayTexH_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> chromeTex_; // 1x1 amber for edit chrome
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromeSRV_;

    // --- OSD sprite state (guarded by overlayMutex_; textures render-thread) ---
    struct OsdSprite {
        std::vector<uint8_t> bgra; // straight-alpha BGRA, tightly packed
        int w = 0;
        int h = 0;
        int x = 0; // top-left in swap-chain pixels
        int y = 0;
        bool dirty = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int texW = 0;
        int texH = 0;
    };
    OsdSprite osdSprites_[kOsdSpriteSlots];
    // Webcam-magnifier dim scrim. Reuses OsdSprite (a generic texture+rect holder)
    // but is composited between the base frame and the webcam PiP, not with the OSD
    // sprites above everything. Guarded by overlayMutex_, like osdSprites_.
    OsdSprite webcamDimScrim_;

    // --- Pushed source mode state ---
    // Handoff from any thread (BeginPushedSource) to the render thread. The render
    // thread exchanges the handle out, opens it, and CloseHandle's it.
    std::atomic<void*> pushedPendingHandle_{nullptr};
    std::atomic<uint32_t> pushedPendingWidth_{0};
    std::atomic<uint32_t> pushedPendingHeight_{0};
    // Display transform for the pending shared surface (PreviewTapTransform as
    // its underlying integer + the ScrgbHdr peak scale). Published before the
    // handle store; read by the render thread after claiming the handle.
    std::atomic<uint8_t> pushedPendingTransform_{0};
    std::atomic<float> pushedPendingPeakScale_{1.0f};
    // True when the pending frames are raw captures (no baked-in overlays).
    std::atomic<bool> pushedPendingRaw_{false};
    std::atomic<bool> pushedRequested_{false};
    // Set by EndPushedSource (any thread); consumed by the render thread, which then
    // reverts to its own WGC capture. A fresh BeginPushedSource clears it so a pending
    // revert never cancels a new recording's handoff.
    std::atomic<bool> pushedEndRequested_{false};
    // Render-thread-owned (no lock; only touched on the render thread).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pushedSharedTex_;         // opened shared surface
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> pushedMutex_;             // keyed mutex on the shared surface
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pushedLocalTex_;          // private copy (decoupled cadence)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pushedLocalSRV_; // SRV over the private copy
    // FP16 scRGB tap only: the render thread's own tone-map pass (the same class
    // the engine's tone-mapped sessions run) and the SDR surface it renders into,
    // which then feeds the ordinary draw instead of pushedLocalSRV_.
    std::unique_ptr<recorder_core::HdrToneMapper> pushedToneMapper_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pushedSdrTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pushedSdrSRV_;
    uint32_t pushedWidth_ = 0;
    uint32_t pushedHeight_ = 0;
    // Pure switch-over state (active / has-frame / wgc-stopped); render-thread-owned.
    PushedSourceState pushed_;

    // --- Snapshot state ---
    std::atomic<bool> snapshotRequested_{false};
    std::mutex snapshotCallbackMutex_;
    SnapshotCallback snapshotCallback_;
    // True once a real source frame has been presented in this renderer run.
    // Set only in RenderFrame() on the render thread; reset only in
    // StartCapture/StartPushedOnly before the render thread is created. No
    // mutex needed: writes and reads never race across those two windows.
    std::atomic<bool> framePresented_{false};
    // Set before StartCapture/StartPushedOnly (no lock); read only on the
    // render thread thereafter.
    std::function<void()> firstFrameCallback_;
    // Render-thread-owned; reallocated only when the source size changes.
    // snapshotRenderTex_/RTV_: full-resolution off-screen target the source SRV
    // is blitted into (unscaled, no letterbox) using the same shader as the
    // on-screen draw. snapshotStagingTex_: CPU-readable copy of that target.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshotRenderTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> snapshotRenderRTV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshotStagingTex_;
    uint32_t snapshotStagingWidth_ = 0;
    uint32_t snapshotStagingHeight_ = 0;
};

} // namespace exosnap
