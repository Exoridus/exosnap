#include "DxgiPreviewRenderer.h"

#include "../diagnostics/AppLog.h"

#include <recorder_core/overlay_shader.h>
#include <recorder_core/webcam_placement.h>

#include <cstring>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// High-resolution waitable timer flag (Windows 10 1803+). Define defensively in
// case the configured SDK headers predate it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace exosnap {
namespace {

constexpr const wchar_t* kChildWindowClass = L"ExoSnapDxgiPreviewChild";

// Keyed-mutex key contract shared with the engine producer (see
// recorder_core/src/preview_shared_texture.h). The mutex starts on the producer
// key; the producer releases to the consumer key ("frame ready") and the consumer
// releases back to the producer key ("free to write"). 0 ms acquires on both
// sides mean neither party ever blocks.
constexpr UINT64 kPushedProducerKey = 0;
constexpr UINT64 kPushedConsumerKey = 1;

// The preview swap chain is SDR BGRA8, so overlay sprites never take the shader's
// HDR-linear branch and the reference-white scale stays neutral.
constexpr bool kPreviewHdrLinear = false;
constexpr float kPreviewRefWhiteScale = 1.0f;

// Every quad the preview draws goes through the shared overlay shader, so it needs
// constants. A frame blit (WGC or pushed engine frame) is an opaque sprite with no
// key and no mirror; so is the amber edit chrome.
[[nodiscard]] recorder_core::OverlayPixelConstants OpaqueQuadConstants() {
    const recorder_core::ChromaKeyParams none; // enabled == false
    return recorder_core::MakeOverlayPixelConstants(none, /*mirror=*/false, /*force_opaque=*/true, /*opacity=*/1.0f,
                                                    kPreviewHdrLinear, kPreviewRefWhiteScale);
}

} // namespace

DxgiPreviewRenderer::DxgiPreviewRenderer() = default;

DxgiPreviewRenderer::~DxgiPreviewRenderer() {
    Shutdown();
}

bool DxgiPreviewRenderer::Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight, uint32_t swapWidth,
                                     uint32_t swapHeight) {
    if (initialized_.load())
        return true;

    parentHwnd_ = parentHwnd;
    initialWidth_ = swapWidth;
    initialHeight_ = swapHeight;

    if (!parentHwnd_) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("initialize failed: null parent HWND"));
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ChildWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kChildWindowClass;

    if (RegisterClassExW(&wc) == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            diagnostics::AppLog::warning(
                QStringLiteral("dxgi-preview"),
                QStringLiteral("RegisterClassExW failed: %1").arg(static_cast<unsigned long>(err)));
            return false;
        }
    }

    childHwnd_ = CreateWindowExW(0, kChildWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                 static_cast<int>(hwndWidth), static_cast<int>(hwndHeight), parentHwnd_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);

    if (!childHwnd_) {
        diagnostics::AppLog::warning(
            QStringLiteral("dxgi-preview"),
            QStringLiteral("CreateWindowExW failed: %1").arg(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    SetWindowPos(childHwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    initialized_.store(true);
    diagnostics::AppLog::debug(QStringLiteral("dxgi-preview"),
                               QStringLiteral("initialized OK parent=0x%1 child=0x%2 hwnd=%3x%4 swap=%5x%6")
                                   .arg(reinterpret_cast<quintptr>(parentHwnd_), 0, 16)
                                   .arg(reinterpret_cast<quintptr>(childHwnd_), 0, 16)
                                   .arg(hwndWidth)
                                   .arg(hwndHeight)
                                   .arg(swapWidth)
                                   .arg(swapHeight));
    return true;
}

bool DxgiPreviewRenderer::StartCapture(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                                       uint32_t frame_rate_den, std::optional<PreviewCropBox> crop_box) {
    if (!initialized_.load())
        return false;

    StopCapture();

    // Store crop box before thread creation so the render thread sees it
    // without synchronization (jthread constructor provides the memory fence).
    cropBox_ = std::move(crop_box);

    // Resolve the captured monitor's virtual-screen rectangle for the cursor
    // sprite (raw pushed frames carry no cursor). Same fence as cropBox_.
    cursorSpriteBoundsValid_ = false;
    if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(target.native_id), &mi) != FALSE) {
            cursorSpriteBounds_ = mi.rcMonitor;
            cursorSpriteBoundsValid_ = true;
        }
    }

    pushedOnlyMode_ = false;

    // Same fence as cropBox_/pushedOnlyMode_ above: reset before the render
    // thread starts so this run's first presented frame fires the callback.
    framePresented_.store(false, std::memory_order_release);

    const uint32_t intervalMs = PreviewFrameIntervalMs(frame_rate_num, frame_rate_den);
    active_.store(true);

    renderThread_ = std::jthread([this, target, intervalMs](std::stop_token st) {
        RenderThreadProc(target, intervalMs, std::move(st));
        active_.store(false);
    });

    return true;
}

bool DxgiPreviewRenderer::StartPushedOnly(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                                          uint32_t frame_rate_den) {
    if (!initialized_.load())
        return false;

    StopCapture();

    cropBox_.reset();

    // The pushed frames will show this monitor; the cursor sprite maps the live
    // pointer against its rectangle. Same fence as StartCapture's cropBox_.
    cursorSpriteBoundsValid_ = false;
    if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(target.native_id), &mi) != FALSE) {
            cursorSpriteBounds_ = mi.rcMonitor;
            cursorSpriteBoundsValid_ = true;
        }
    }

    pushedOnlyMode_ = true;

    // Same fence as cropBox_/pushedOnlyMode_ above: reset before the render
    // thread starts so this run's first presented frame fires the callback.
    framePresented_.store(false, std::memory_order_release);

    const uint32_t intervalMs = PreviewFrameIntervalMs(frame_rate_num, frame_rate_den);
    active_.store(true);

    renderThread_ = std::jthread([this, target, intervalMs](std::stop_token st) {
        RenderThreadProc(target, intervalMs, std::move(st));
        active_.store(false);
    });

    return true;
}

void DxgiPreviewRenderer::StopCapture() {
    active_.store(false);
    if (renderThread_.joinable()) {
        renderThread_.request_stop();
        renderThread_.join();
    }
}

void DxgiPreviewRenderer::Resize(uint32_t hwndWidth, uint32_t hwndHeight, uint32_t swapWidth, uint32_t swapHeight) {
    if (!childHwnd_)
        return;

    SetWindowPos(childHwnd_, nullptr, 0, 0, static_cast<int>(hwndWidth), static_cast<int>(hwndHeight),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    requestedWidth_.store(swapWidth);
    requestedHeight_.store(swapHeight);
    resizeRequested_.store(true);
}

bool DxgiPreviewRenderer::IsActive() const noexcept {
    return active_.load();
}

void DxgiPreviewRenderer::GetSourceSize(uint32_t& outWidth, uint32_t& outHeight) const noexcept {
    std::lock_guard lock(frameMutex_);
    outWidth = srcWidth_;
    outHeight = srcHeight_;
}

void DxgiPreviewRenderer::SetWebcamOverlayState(bool enabled, bool selected, float nx, float ny, float nw, float nh,
                                                bool mirror, float opacity,
                                                const recorder_core::ChromaKeyParams& chroma) {
    std::lock_guard lock(overlayMutex_);
    overlayEnabled_ = enabled;
    overlaySelected_ = selected;
    overlayNx_ = nx;
    overlayNy_ = ny;
    overlayNw_ = nw;
    overlayNh_ = nh;
    overlayOpacity_ = std::isfinite(static_cast<double>(opacity)) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    // Mirror and chroma reach the GPU as shader constants, so neither invalidates
    // the uploaded texture.
    overlayMirror_ = mirror;
    overlayChroma_ = chroma;
}

void DxgiPreviewRenderer::SetWebcamOverlayFrame(const uint8_t* bgra, int width, int height, int stride) {
    std::lock_guard lock(overlayMutex_);
    if (bgra == nullptr || width <= 0 || height <= 0) {
        overlayBgra_.clear();
        overlayW_ = 0;
        overlayH_ = 0;
        overlayDirty_ = true;
        return;
    }
    if (stride <= 0)
        stride = width * 4;
    const int rowBytes = width * 4;
    overlayBgra_.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        std::memcpy(overlayBgra_.data() + static_cast<size_t>(y) * rowBytes,
                    bgra + static_cast<size_t>(y) * static_cast<size_t>(stride), static_cast<size_t>(rowBytes));
    }
    overlayW_ = width;
    overlayH_ = height;
    overlayDirty_ = true;
}

void DxgiPreviewRenderer::Shutdown() {
    EndPushedSource();
    StopCapture();

    if (childHwnd_) {
        DestroyWindow(childHwnd_);
        childHwnd_ = nullptr;
    }

    initialized_.store(false);
}

// --- Pushed source mode ---

void DxgiPreviewRenderer::BeginPushedSource(void* nt_handle, uint32_t width, uint32_t height,
                                            recorder_core::PreviewTapDesc tap, bool raw_source_frames) {
    if (nt_handle == nullptr || width == 0 || height == 0)
        return;
    // Store dimensions + transform first, then the handle with release ordering:
    // the render thread reads the handle with acquire ordering and then these.
    pushedPendingWidth_.store(width, std::memory_order_relaxed);
    pushedPendingHeight_.store(height, std::memory_order_relaxed);
    pushedPendingTransform_.store(static_cast<uint8_t>(tap.transform), std::memory_order_relaxed);
    pushedPendingPeakScale_.store(tap.peak_scale, std::memory_order_relaxed);
    pushedPendingRaw_.store(raw_source_frames, std::memory_order_relaxed);
    // A fresh handoff cancels any pending revert from the previous recording.
    pushedEndRequested_.store(false, std::memory_order_release);
    // If a prior pending handle was never adopted, close it before overwriting.
    void* prior = pushedPendingHandle_.exchange(nt_handle, std::memory_order_acq_rel);
    if (prior != nullptr && prior != nt_handle)
        CloseHandle(static_cast<HANDLE>(prior));
    pushedRequested_.store(true, std::memory_order_release);
}

void DxgiPreviewRenderer::EndPushedSource() {
    pushedRequested_.store(false, std::memory_order_release);
    // Drain a pending handle that the render thread never adopted (e.g. recording
    // ended before the first engine frame).
    void* stale = pushedPendingHandle_.exchange(nullptr, std::memory_order_acq_rel);
    if (stale != nullptr)
        CloseHandle(static_cast<HANDLE>(stale));
    // Signal the render thread to leave pushed mode and rebuild its own WGC capture
    // in place. This is what actually un-freezes the preview after recording: the
    // render loop stays alive in pushed mode, so a caller-side teardown alone would
    // never restart the WGC graph.
    pushedEndRequested_.store(true, std::memory_order_release);
}

void DxgiPreviewRenderer::StopCaptureGraph() {
    if (captureSession_) {
        captureSession_.Close();
        captureSession_ = nullptr;
    }
    if (framePool_) {
        framePool_.Close();
        framePool_ = nullptr;
    }
    if (captureItem_) {
        captureItem_.Closed(closedToken_);
        closedToken_ = {};
        captureItem_ = nullptr;
    }
    // A stale source-lost flag from the closed graph must not end the render loop
    // while pushed mode is running.
    sourceLost_.store(false);
}

void DxgiPreviewRenderer::AdoptPendingPushedSource() {
    void* pending = pushedPendingHandle_.load(std::memory_order_acquire);
    if (pending == nullptr)
        return;
    // Claim it (CAS guards against a concurrent EndPushedSource drain).
    if (!pushedPendingHandle_.compare_exchange_strong(pending, nullptr, std::memory_order_acq_rel))
        return;

    const uint32_t w = pushedPendingWidth_.load(std::memory_order_relaxed);
    const uint32_t h = pushedPendingHeight_.load(std::memory_order_relaxed);

    // Opening a new handle supersedes any previously opened one (session restart,
    // or an idle-hub -> engine handover). Keep the last presented image as the
    // fallback frame so the swap shows a hold instead of a black flash — in
    // pushed-only mode there is no WGC image to fall back to. The displayable
    // surface is the tone-mapped SDR one when the old source was FP16.
    if (pushedLocalTex_ && pushedWidth_ > 0 && pushedHeight_ > 0) {
        std::lock_guard lock(frameMutex_);
        latestFrame_ = pushedSdrTex_ ? pushedSdrTex_ : pushedLocalTex_;
        latestFrameSRV_ = pushedSdrSRV_ ? pushedSdrSRV_ : pushedLocalSRV_;
        srcWidth_ = pushedWidth_;
        srcHeight_ = pushedHeight_;
    }
    ReleasePushedResources();

    // Track the actual failing stage + HRESULT so the log points at the real cause
    // (opening the shared handle vs. creating the local texture/SRV), not always at
    // OpenSharedResource1.
    bool opened = false;
    HRESULT failHr = E_FAIL;
    const char* failStage = "device-query";
    Microsoft::WRL::ComPtr<ID3D11Device1> dev1;
    HRESULT devHr = d3dDevice_ ? d3dDevice_->QueryInterface(IID_PPV_ARGS(dev1.GetAddressOf())) : E_POINTER;
    if (SUCCEEDED(devHr) && dev1) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex;
        HRESULT openHr =
            dev1->OpenSharedResource1(static_cast<HANDLE>(pending), IID_PPV_ARGS(sharedTex.GetAddressOf()));
        if (FAILED(openHr) || !sharedTex) {
            failStage = "OpenSharedResource1";
            failHr = FAILED(openHr) ? openHr : E_POINTER;
        } else {
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> km;
            HRESULT kmHr = sharedTex->QueryInterface(IID_PPV_ARGS(km.GetAddressOf()));
            if (FAILED(kmHr)) {
                failStage = "keyed-mutex-query";
                failHr = kmHr;
            } else {
                D3D11_TEXTURE2D_DESC td{};
                sharedTex->GetDesc(&td);
                // Private copy target (default usage, shader-readable) so the
                // present cadence is decoupled from the producer's write cadence.
                D3D11_TEXTURE2D_DESC ld = td;
                ld.MiscFlags = 0;
                ld.CPUAccessFlags = 0;
                ld.Usage = D3D11_USAGE_DEFAULT;
                ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> localTex;
                HRESULT texHr = d3dDevice_->CreateTexture2D(&ld, nullptr, localTex.GetAddressOf());
                if (FAILED(texHr)) {
                    failStage = "local-texture";
                    failHr = texHr;
                } else {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = td.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                    HRESULT srvHr = d3dDevice_->CreateShaderResourceView(localTex.Get(), &srvDesc, srv.GetAddressOf());
                    if (FAILED(srvHr)) {
                        failStage = "local-srv";
                        failHr = srvHr;
                    } else {
                        // FP16 scRGB tap (native HDR10 session): build this thread's
                        // own tone-map pass and the SDR surface the draw samples
                        // instead of the raw FP16 copy. Any failure here fails the
                        // adopt as a whole, so the preview keeps its own WGC capture
                        // (same fallback as a cross-GPU open failure).
                        const auto transform = static_cast<recorder_core::PreviewTapTransform>(
                            pushedPendingTransform_.load(std::memory_order_relaxed));
                        const float peakScale = pushedPendingPeakScale_.load(std::memory_order_relaxed);
                        std::unique_ptr<recorder_core::HdrToneMapper> toneMapper;
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> sdrTex;
                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sdrSrv;
                        bool toneMapOk = true;
                        if (transform != recorder_core::PreviewTapTransform::None) {
                            toneMapOk = false;
                            if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                failStage = "tap-transform-format";
                                failHr = E_INVALIDARG;
                            } else {
                                D3D11_TEXTURE2D_DESC sd = td;
                                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                sd.MiscFlags = 0;
                                sd.CPUAccessFlags = 0;
                                sd.Usage = D3D11_USAGE_DEFAULT;
                                sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                                HRESULT sdrHr = d3dDevice_->CreateTexture2D(&sd, nullptr, sdrTex.GetAddressOf());
                                HRESULT sdrSrvHr = E_FAIL;
                                if (FAILED(sdrHr)) {
                                    failStage = "tonemap-target";
                                    failHr = sdrHr;
                                } else if (sdrSrvHr = d3dDevice_->CreateShaderResourceView(sdrTex.Get(), nullptr,
                                                                                           sdrSrv.GetAddressOf());
                                           FAILED(sdrSrvHr)) {
                                    failStage = "tonemap-srv";
                                    failHr = sdrSrvHr;
                                } else {
                                    toneMapper = std::make_unique<recorder_core::HdrToneMapper>();
                                    std::string tmErr;
                                    const bool sdrScrgb = transform == recorder_core::PreviewTapTransform::ScrgbSdr;
                                    if (!toneMapper->Init(d3dDevice_.Get(), d3dContext_.Get(), td.Width, td.Height,
                                                          peakScale, sdrScrgb, tmErr)) {
                                        failStage = "tonemap-init";
                                        failHr = E_FAIL;
                                        toneMapper.reset();
                                    } else {
                                        toneMapOk = true;
                                    }
                                }
                            }
                        }
                        if (toneMapOk) {
                            pushedSharedTex_ = sharedTex;
                            pushedMutex_ = km;
                            pushedLocalTex_ = localTex;
                            pushedLocalSRV_ = srv;
                            pushedToneMapper_ = std::move(toneMapper);
                            pushedSdrTex_ = sdrTex;
                            pushedSdrSRV_ = sdrSrv;
                            pushedWidth_ = w;
                            pushedHeight_ = h;
                            // Engine frames arrive with cursor + webcam PiP baked in
                            // exactly as recorded; raw hub frames carry neither and
                            // the renderer draws both itself.
                            pushed_.OnSourceOpened(pushedPendingRaw_.load(std::memory_order_relaxed));
                            opened = true;
                            diagnostics::AppLog::debug(
                                QStringLiteral("dxgi-preview"),
                                QStringLiteral("pushed source active %1x%2 on preview device%3")
                                    .arg(w)
                                    .arg(h)
                                    .arg(pushedToneMapper_ ? QStringLiteral(" (FP16 tone-map)") : QString{}));
                        }
                    }
                }
            }
        }
    } else {
        failStage = "device-query";
        failHr = devHr;
    }
    if (!opened) {
        // The adapter-mismatch hint only applies to the handle-open stage; a
        // texture/SRV failure is a local allocation problem, not a cross-GPU one.
        const QString hint = (std::strcmp(failStage, "OpenSharedResource1") == 0)
                                 ? QStringLiteral(" (adapter mismatch or cross-GPU?)")
                                 : QString{};
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("pushed source: %1 failed 0x%2%3")
                                         .arg(QLatin1String(failStage))
                                         .arg(static_cast<unsigned long>(failHr), 8, 16, QChar('0'))
                                         .arg(hint));
    }
    // Ownership was handed to this thread; always close the NT handle.
    CloseHandle(static_cast<HANDLE>(pending));
}

void DxgiPreviewRenderer::RevertToWgcCapture(const recorder_core::CaptureTarget& target) {
    // Symmetric inverse of AdoptPendingPushedSource + StopCaptureGraph: after
    // recording stops, drop the engine's shared resources and bring the preview's
    // OWN WGC capture back — in place, without tearing down the D3D device/swap
    // chain (no black flash). Render-thread only.
    const bool needsWgcRebuild = pushed_.NeedsWgcRebuildOnRevert();
    ReleasePushedResources(); // releases shared/local textures and resets pushed_

    if (!needsWgcRebuild)
        return; // pushed mode never stopped the WGC graph (e.g. open failed) — it is
                // still running, so there is nothing to rebuild.

    // The WGC graph was closed when pushed mode began; rebuild it. latestFrame_ still
    // holds the last WGC image, so the preview shows that until the first fresh WGC
    // frame lands (no black flash). Clear the stale source-lost flag first so the
    // render loop keeps running against the rebuilt graph.
    sourceLost_.store(false);
    if (!InitCaptureItem(target) || !InitFramePool()) {
        diagnostics::AppLog::warning(
            QStringLiteral("dxgi-preview"),
            QStringLiteral("pushed revert: WGC capture rebuild failed; preview holds last frame"));
    } else {
        diagnostics::AppLog::debug(QStringLiteral("dxgi-preview"),
                                   QStringLiteral("pushed revert: WGC capture rebuilt in place"));
    }
}

void DxgiPreviewRenderer::ConsumePushedFrame() {
    if (!pushedMutex_ || !pushedLocalTex_ || !pushedSharedTex_)
        return;
    // 0 ms acquire: if the producer currently holds the mutex we keep the last
    // local copy for this present tick rather than stall the render thread.
    if (pushedMutex_->AcquireSync(kPushedConsumerKey, 0) == S_OK) {
        d3dContext_->CopyResource(pushedLocalTex_.Get(), pushedSharedTex_.Get());
        pushedMutex_->ReleaseSync(kPushedProducerKey);
        // FP16 scRGB tap: tone-map the fresh copy down to the SDR surface the
        // draw samples. Runs once per consumed frame, not per present tick.
        if (pushedToneMapper_ && pushedSdrTex_) {
            std::string tmErr;
            if (!pushedToneMapper_->Convert(pushedLocalTex_.Get(), pushedSdrTex_.Get(), tmErr)) {
                diagnostics::AppLog::warning(
                    QStringLiteral("dxgi-preview"),
                    QStringLiteral("pushed tone-map failed: %1").arg(QString::fromStdString(tmErr)));
            }
        }
        pushed_.OnFrameConsumed();
    }
}

void DxgiPreviewRenderer::ReleasePushedResources() {
    pushedToneMapper_.reset();
    pushedSdrSRV_.Reset();
    pushedSdrTex_.Reset();
    pushedLocalSRV_.Reset();
    pushedLocalTex_.Reset();
    pushedMutex_.Reset();
    pushedSharedTex_.Reset();
    pushedWidth_ = 0;
    pushedHeight_ = 0;
    pushed_.Reset();
}

LRESULT CALLBACK DxgiPreviewRenderer::ChildWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool DxgiPreviewRenderer::InitD3D11() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, d3dDevice_.GetAddressOf(),
                                   nullptr, d3dContext_.GetAddressOf());

    if (FAILED(hr)) {
        diagnostics::AppLog::warning(
            QStringLiteral("dxgi-preview"),
            QStringLiteral("D3D11CreateDevice failed: 0x%1").arg(static_cast<unsigned long>(hr), 8, 16));
        return false;
    }
    return true;
}

bool DxgiPreviewRenderer::InitSwapChain(uint32_t width, uint32_t height) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
    if (FAILED(hr))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
    if (FAILED(hr))
        return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
    if (FAILED(hr))
        return false;

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice_.Get(), childHwnd_, &scDesc, nullptr, nullptr,
                                             swapChain_.GetAddressOf());
    if (FAILED(hr)) {
        diagnostics::AppLog::warning(
            QStringLiteral("dxgi-preview"),
            QStringLiteral("CreateSwapChainForHwnd failed: 0x%1").arg(static_cast<unsigned long>(hr), 8, 16));
        return false;
    }

    swapWidth_ = width;
    swapHeight_ = height;
    return true;
}

bool DxgiPreviewRenderer::InitShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr =
        D3DCompile(recorder_core::kOverlayVertexShaderSrc, strlen(recorder_core::kOverlayVertexShaderSrc), "vs_main",
                   nullptr, nullptr, "main", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob)
            diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                         QStringLiteral("vertex shader compile failed"));
        return false;
    }

    hr = D3DCompile(recorder_core::kOverlayPixelShaderSrc, strlen(recorder_core::kOverlayPixelShaderSrc), "ps_main",
                    nullptr, nullptr, "main", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob)
            diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"), QStringLiteral("pixel shader compile failed"));
        return false;
    }

    hr = d3dDevice_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                        vertexShader_.GetAddressOf());
    if (FAILED(hr))
        return false;

    hr = d3dDevice_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                       pixelShader_.GetAddressOf());
    if (FAILED(hr))
        return false;

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = d3dDevice_->CreateSamplerState(&sampDesc, samplerState_.GetAddressOf());
    if (FAILED(hr))
        return false;

    // Straight source-alpha blend, identical to the recording compositor's. The
    // shader writes the final alpha (chroma key × uniform opacity), so a single
    // state covers a keyed edge, a translucent PiP and the opaque edit chrome.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = d3dDevice_->CreateBlendState(&bd, overlayBlendState_.GetAddressOf());
    if (FAILED(hr))
        return false;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(recorder_core::OverlayPixelConstants);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, constantBuffer_.GetAddressOf());
    if (FAILED(hr))
        return false;

    return true;
}

bool DxgiPreviewRenderer::InitCaptureItem(const recorder_core::CaptureTarget& target) {
    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();

        if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
            winrt::check_hresult(
                interop->CreateForMonitor(reinterpret_cast<HMONITOR>(target.native_id),
                                          winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                          winrt::put_abi(captureItem_)));
        } else {
            winrt::check_hresult(
                interop->CreateForWindow(reinterpret_cast<HWND>(target.native_id),
                                         winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                                         winrt::put_abi(captureItem_)));
        }
    } catch (...) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"), QStringLiteral("create capture item failed"));
        return false;
    }

    if (!captureItem_) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("capture item is null after creation"));
        return false;
    }

    auto sz = captureItem_.Size();
    diagnostics::AppLog::debug(QStringLiteral("dxgi-preview"),
                               QStringLiteral("capture item created OK size=%1x%2").arg(sz.Width).arg(sz.Height));
    return true;
}

bool DxgiPreviewRenderer::InitFramePool() {
    try {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT qiHr = d3dDevice_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
        if (FAILED(qiHr))
            return false;
        winrt::com_ptr<IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), insp.put()));
        auto d3dWinRTDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto sz = captureItem_.Size();
        framePool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
            d3dWinRTDev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, sz);

        captureSession_ = framePool_.CreateCaptureSession(captureItem_);
        captureSession_.IsBorderRequired(false);
        captureSession_.StartCapture();

        closedToken_ = captureItem_.Closed([this](const auto&, const auto&) { sourceLost_.store(true); });
    } catch (...) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"), QStringLiteral("InitFramePool failed"));
        return false;
    }
    return true;
}

void DxgiPreviewRenderer::CleanupCapture() {
    if (captureSession_) {
        captureSession_.Close();
        captureSession_ = nullptr;
    }
    if (framePool_) {
        framePool_.Close();
        framePool_ = nullptr;
    }
    if (captureItem_) {
        captureItem_.Closed(closedToken_);
        closedToken_ = {};
        captureItem_ = nullptr;
    }
    swapChain_.Reset();
    vertexShader_.Reset();
    pixelShader_.Reset();
    samplerState_.Reset();
    overlayBlendState_.Reset();
    {
        std::lock_guard lock(overlayMutex_);
        overlayTex_.Reset();
        overlaySRV_.Reset();
        overlayTexW_ = 0;
        overlayTexH_ = 0;
        overlayDirty_ = true; // force re-upload if a new capture starts
        chromeTex_.Reset();
        chromeSRV_.Reset();
    }
    // Pushed-source resources live on this device; release before the device.
    ReleasePushedResources();
    // Cursor-sprite cache lives on this device too.
    cursorSpriteTex_.Reset();
    cursorSpriteSRV_.Reset();
    cursorSpriteTexClip_ = {};
    cursorSpriteHandle_ = nullptr;
    cursorSpriteBitmap_ = {};
    // Drain any handle that was signalled but never adopted (avoid a handle leak).
    void* stalePushed = pushedPendingHandle_.exchange(nullptr, std::memory_order_acq_rel);
    if (stalePushed != nullptr)
        CloseHandle(static_cast<HANDLE>(stalePushed));

    d3dContext_.Reset();
    d3dDevice_.Reset();
    latestFrame_.Reset();
    latestFrameSRV_.Reset();
}

void DxgiPreviewRenderer::PollAndProcessFrames() {
    if (!framePool_)
        return;

    try {
        while (true) {
            auto frame = framePool_.TryGetNextFrame();
            if (frame == nullptr)
                break;

            std::lock_guard lock(frameMutex_);

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

            Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTex;
            HRESULT hr = access->GetInterface(IID_PPV_ARGS(frameTex.GetAddressOf()));
            if (FAILED(hr))
                continue;

            D3D11_TEXTURE2D_DESC desc{};
            frameTex->GetDesc(&desc);

            // When a crop box is active (Region preview), render only the selected
            // sub-region.  srcWidth_/srcHeight_ track the *effective* dimensions so
            // ComputeContainFitRect receives the crop aspect ratio, not the full monitor
            // dimensions.
            const uint32_t effectiveW = cropBox_.has_value() ? static_cast<uint32_t>(cropBox_->width) : desc.Width;
            const uint32_t effectiveH = cropBox_.has_value() ? static_cast<uint32_t>(cropBox_->height) : desc.Height;

            bool sizeChanged = (effectiveW != srcWidth_ || effectiveH != srcHeight_);
            srcWidth_ = effectiveW;
            srcHeight_ = effectiveH;

            if (sizeChanged || !latestFrame_) {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                // Texture sized to the cropped dimensions; the shader renders from (0,0).
                copyDesc.Width = effectiveW;
                copyDesc.Height = effectiveH;
                copyDesc.MiscFlags = 0;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                latestFrame_.Reset();
                latestFrameSRV_.Reset();

                hr = d3dDevice_->CreateTexture2D(&copyDesc, nullptr, latestFrame_.GetAddressOf());
                if (FAILED(hr))
                    continue;

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = desc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                hr = d3dDevice_->CreateShaderResourceView(latestFrame_.Get(), &srvDesc, latestFrameSRV_.GetAddressOf());
                if (FAILED(hr))
                    continue;
            }

            if (cropBox_.has_value()) {
                // Clamp the crop box to the actual frame bounds to prevent out-of-range
                // D3D11 access if the monitor resolution changed since the box was set.
                const uint32_t cx = static_cast<uint32_t>(std::max(0, cropBox_->x));
                const uint32_t cy = static_cast<uint32_t>(std::max(0, cropBox_->y));
                const uint32_t maxCW = (cx < desc.Width) ? (desc.Width - cx) : 0u;
                const uint32_t maxCH = (cy < desc.Height) ? (desc.Height - cy) : 0u;
                const uint32_t cw = std::min(effectiveW, maxCW);
                const uint32_t ch = std::min(effectiveH, maxCH);
                if (cw > 0 && ch > 0) {
                    // Copy only the selected region into the crop-sized latestFrame_.
                    D3D11_BOX srcBox{};
                    srcBox.left = cx;
                    srcBox.top = cy;
                    srcBox.right = cx + cw;
                    srcBox.bottom = cy + ch;
                    srcBox.front = 0;
                    srcBox.back = 1;
                    d3dContext_->CopySubresourceRegion(latestFrame_.Get(), 0, 0, 0, 0, frameTex.Get(), 0, &srcBox);
                }
            } else {
                d3dContext_->CopySubresourceRegion(latestFrame_.Get(), 0, 0, 0, 0, frameTex.Get(), 0, nullptr);
            }
        }
    } catch (...) {
    }
}

void DxgiPreviewRenderer::ResizeSwapChainInternal(uint32_t width, uint32_t height) {
    if (!swapChain_)
        return;
    if (width < 1 || height < 1)
        return;

    d3dContext_->OMSetRenderTargets(0, nullptr, nullptr);
    d3dContext_->Flush();

    swapChain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    swapWidth_ = width;
    swapHeight_ = height;
}

void DxgiPreviewRenderer::EnsureChromeTexture() {
    if (chromeSRV_)
        return;
    // 1x1 amber (BGRA) used as a solid fill for the PiP edit border + handles.
    const uint8_t amber[4] = {0x44, 0xa7, 0xd7, 0xff}; // B,G,R,A == #d7a744
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 1;
    d.Height = 1;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = amber;
    init.SysMemPitch = 4;
    if (FAILED(d3dDevice_->CreateTexture2D(&d, &init, chromeTex_.GetAddressOf())))
        return;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = d.Format;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MipLevels = 1;
    if (FAILED(d3dDevice_->CreateShaderResourceView(chromeTex_.Get(), &s, chromeSRV_.GetAddressOf())))
        chromeTex_.Reset();
}

void DxgiPreviewRenderer::UploadOverlayTexture() {
    // Caller holds overlayMutex_.
    if (overlayBgra_.empty() || overlayW_ <= 0 || overlayH_ <= 0) {
        overlaySRV_.Reset();
        overlayTex_.Reset();
        overlayTexW_ = 0;
        overlayTexH_ = 0;
        return;
    }
    if (!overlayTex_ || overlayTexW_ != overlayW_ || overlayTexH_ != overlayH_) {
        overlayTex_.Reset();
        overlaySRV_.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(overlayW_);
        d.Height = static_cast<UINT>(overlayH_);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d3dDevice_->CreateTexture2D(&d, nullptr, overlayTex_.GetAddressOf())))
            return;
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = d.Format;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = 1;
        if (FAILED(d3dDevice_->CreateShaderResourceView(overlayTex_.Get(), &s, overlaySRV_.GetAddressOf()))) {
            overlayTex_.Reset();
            return;
        }
        overlayTexW_ = overlayW_;
        overlayTexH_ = overlayH_;
    }

    // Uploaded unmirrored; the shader flips the texcoord (params.x), exactly as the
    // recording compositor does.
    const UINT rowBytes = static_cast<UINT>(overlayW_) * 4u;
    d3dContext_->UpdateSubresource(overlayTex_.Get(), 0, nullptr, overlayBgra_.data(), rowBytes, 0);
}

void DxgiPreviewRenderer::RenderWebcamOverlay(int contentX, int contentY, int contentW, int contentH) {
    if (contentW <= 0 || contentH <= 0)
        return;

    std::lock_guard lock(overlayMutex_);
    if (!overlayEnabled_)
        return;
    if (overlayDirty_) {
        UploadOverlayTexture();
        overlayDirty_ = false;
    }
    if (!overlaySRV_ || overlayW_ <= 0 || overlayH_ <= 0)
        return;

    // Map the normalized placement onto the content rect via the shared helper —
    // the exact same math the recording compositor uses on the encode frame.
    recorder_core::WebcamPlacement placement;
    placement.x = overlayNx_;
    placement.y = overlayNy_;
    placement.w = overlayNw_;
    placement.h = overlayNh_;
    placement.mirror = overlayMirror_; // geometry only; the flip happens in the shader
    const recorder_core::WebcamPixelRect r =
        recorder_core::MapWebcamPlacementToContent(placement, contentX, contentY, contentW, contentH);
    if (!r.IsValid())
        return;

    d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto setConstants = [&](const recorder_core::OverlayPixelConstants& pc) {
        d3dContext_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &pc, 0, 0);
        d3dContext_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    };

    auto drawQuad = [&](ID3D11ShaderResourceView* srv, int x, int y, int w, int h) {
        if (w <= 0 || h <= 0)
            return;
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<float>(x);
        vp.TopLeftY = static_cast<float>(y);
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        d3dContext_->RSSetViewports(1, &vp);
        d3dContext_->PSSetShaderResources(0, 1, &srv);
        d3dContext_->Draw(3, 0);
    };

    // PiP video (stretched to the rect — same as the recording compositor). One pass
    // carries the chroma key, the mirror and the opacity; alpha out of the shader
    // drives the blend, so a keyed background and a translucent PiP compose together.
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    d3dContext_->OMSetBlendState(overlayBlendState_.Get(), blendFactor, 0xffffffff);
    setConstants(recorder_core::MakeOverlayPixelConstants(overlayChroma_, overlayMirror_, /*force_opaque=*/true,
                                                          overlayOpacity_, kPreviewHdrLinear, kPreviewRefWhiteScale));
    drawQuad(overlaySRV_.Get(), r.x, r.y, r.w, r.h);

    // Edit chrome (border + corner handles) only while selected — never keyed, never
    // dimmed by the PiP opacity.
    if (overlaySelected_) {
        EnsureChromeTexture();
        if (chromeSRV_) {
            setConstants(OpaqueQuadConstants());
            ID3D11ShaderResourceView* c = chromeSRV_.Get();
            constexpr int t = 2;  // border thickness
            constexpr int hs = 8; // handle size
            drawQuad(c, r.x, r.y, r.w, t);
            drawQuad(c, r.x, r.y + r.h - t, r.w, t);
            drawQuad(c, r.x, r.y, t, r.h);
            drawQuad(c, r.x + r.w - t, r.y, t, r.h);
            drawQuad(c, r.x - hs / 2, r.y - hs / 2, hs, hs);
            drawQuad(c, r.x + r.w - hs / 2, r.y - hs / 2, hs, hs);
            drawQuad(c, r.x - hs / 2, r.y + r.h - hs / 2, hs, hs);
            drawQuad(c, r.x + r.w - hs / 2, r.y + r.h - hs / 2, hs, hs);
        }
    }

    // The background blit of the next frame draws unblended.
    d3dContext_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

void DxgiPreviewRenderer::RenderCursorSprite(int contentX, int contentY, int contentW, int contentH) {
    if (contentW <= 0 || contentH <= 0 || !cursorSpriteBoundsValid_ || pushedWidth_ == 0 || pushedHeight_ == 0)
        return;

    CURSORINFO cursorInfo{};
    cursorInfo.cbSize = sizeof(cursorInfo);
    if (GetCursorInfo(&cursorInfo) == FALSE || (cursorInfo.flags & CURSOR_SHOWING) == 0 ||
        cursorInfo.hCursor == nullptr)
        return;

    if (cursorInfo.hCursor != cursorSpriteHandle_ || cursorSpriteBitmap_.bgra.empty()) {
        recorder_core::Win32CursorBitmap next;
        if (!recorder_core::CaptureWin32CursorBitmap(cursorInfo.hCursor, next))
            return;
        cursorSpriteHandle_ = cursorInfo.hCursor;
        cursorSpriteBitmap_ = std::move(next);
        cursorSpriteTex_.Reset();
        cursorSpriteSRV_.Reset();
        cursorSpriteTexClip_ = {};
    }

    const int32_t boundsW = static_cast<int32_t>(cursorSpriteBounds_.right - cursorSpriteBounds_.left);
    const int32_t boundsH = static_cast<int32_t>(cursorSpriteBounds_.bottom - cursorSpriteBounds_.top);
    if (boundsW <= 0 || boundsH <= 0)
        return;

    // Screen position -> source-frame pixels (1:1 for a native-resolution monitor
    // duplication; the shared helper covers a mismatch), hotspot-adjusted. The
    // same arithmetic the recording compositor runs, so the preview's sprite
    // lands where the recorded one would.
    const auto srcW = static_cast<int32_t>(pushedWidth_);
    const auto srcH = static_cast<int32_t>(pushedHeight_);
    const int32_t sx = recorder_core::ScaleCoordinateToSource(
                           static_cast<int32_t>(cursorInfo.ptScreenPos.x - cursorSpriteBounds_.left), srcW, boundsW) -
                       cursorSpriteBitmap_.hotspot_x;
    const int32_t sy = recorder_core::ScaleCoordinateToSource(
                           static_cast<int32_t>(cursorInfo.ptScreenPos.y - cursorSpriteBounds_.top), srcH, boundsH) -
                       cursorSpriteBitmap_.hotspot_y;

    const recorder_core::CursorSpriteDraw draw = recorder_core::PlaceCursorSprite(
        sx, sy, cursorSpriteBitmap_.width, cursorSpriteBitmap_.height, srcW, srcH, static_cast<float>(contentX),
        static_cast<float>(contentY), static_cast<float>(contentW), static_cast<float>(contentH));
    if (!draw.visible)
        return;

    // (Re)upload the cropped sprite region only when the crop changed. Fully
    // inside the source (the common case) the crop equals the whole bitmap and
    // the cached texture holds; the per-frame recreation happens only while the
    // cursor slides along a source edge.
    if (!cursorSpriteSRV_ || draw.clip.w != cursorSpriteTexClip_.w || draw.clip.h != cursorSpriteTexClip_.h ||
        draw.clip.bitmap_off_x != cursorSpriteTexClip_.bitmap_off_x ||
        draw.clip.bitmap_off_y != cursorSpriteTexClip_.bitmap_off_y) {
        std::vector<uint8_t> cropped(static_cast<size_t>(draw.clip.w) * draw.clip.h * 4u);
        for (int32_t row = 0; row < draw.clip.h; ++row) {
            const size_t srcOff = (static_cast<size_t>(draw.clip.bitmap_off_y + row) * cursorSpriteBitmap_.width +
                                   draw.clip.bitmap_off_x) *
                                  4u;
            std::memcpy(cropped.data() + static_cast<size_t>(row) * draw.clip.w * 4u,
                        cursorSpriteBitmap_.bgra.data() + srcOff, static_cast<size_t>(draw.clip.w) * 4u);
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(draw.clip.w);
        td.Height = static_cast<UINT>(draw.clip.h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = cropped.data();
        init.SysMemPitch = static_cast<UINT>(draw.clip.w) * 4u;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(d3dDevice_->CreateTexture2D(&td, &init, tex.GetAddressOf())) ||
            FAILED(d3dDevice_->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf())))
            return;
        cursorSpriteTex_ = std::move(tex);
        cursorSpriteSRV_ = std::move(srv);
        cursorSpriteTexClip_ = draw.clip;
    }

    // Draw through the shared overlay shader as a cursor sprite: source alpha
    // preserved (OverlayMode::Cursor), no chroma key, no mirror, full opacity.
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    d3dContext_->OMSetBlendState(overlayBlendState_.Get(), blendFactor, 0xffffffff);
    const recorder_core::OverlayPixelConstants pc = recorder_core::MakeOverlayPixelConstants(
        recorder_core::ChromaKeyParams{}, /*mirror=*/false, /*force_opaque=*/false, /*opacity=*/1.0f,
        /*hdr_linear=*/false, /*ref_white_scale=*/1.0f);
    d3dContext_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &pc, 0, 0);

    d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
    d3dContext_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = draw.dst_x;
    vp.TopLeftY = draw.dst_y;
    vp.Width = draw.dst_w;
    vp.Height = draw.dst_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    d3dContext_->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* srv = cursorSpriteSRV_.Get();
    d3dContext_->PSSetShaderResources(0, 1, &srv);
    d3dContext_->Draw(3, 0);

    d3dContext_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

void DxgiPreviewRenderer::RenderFrame() {
    if (!swapChain_ || !d3dContext_)
        return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr))
        return;

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    hr = d3dDevice_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr))
        return;

    d3dContext_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    d3dContext_->ClearRenderTargetView(rtv.Get(), clearColor);

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    // Content rectangle of the main frame inside the backbuffer (contain-fit). The
    // PiP overlay is placed relative to this so it never lands in letterbox margins.
    LONG contentX = 0, contentY = 0, contentW = 0, contentH = 0;

    // While a pushed engine frame is available, that frame is the background (it
    // already contains the cursor + webcam PiP exactly as recorded). Until the
    // first pushed frame lands we fall back to the last WGC image (no black flash).
    const bool drawPushed = pushed_.DrawsPushedBackground() && pushedLocalSRV_ && pushedWidth_ > 0 && pushedHeight_ > 0;

    auto drawSource = [&](ID3D11ShaderResourceView* srv, uint32_t srcW, uint32_t srcH) {
        LONG dx = 0, dy = 0, dw = static_cast<LONG>(bbDesc.Width), dh = static_cast<LONG>(bbDesc.Height);
        ComputeContainFitRect(static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height),
                              static_cast<LONG>(srcW), static_cast<LONG>(srcH), dx, dy, dw, dh);
        if (dw <= 0 || dh <= 0)
            return;
        contentX = dx;
        contentY = dy;
        contentW = dw;
        contentH = dh;

        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<float>(dx);
        vp.TopLeftY = static_cast<float>(dy);
        vp.Width = static_cast<float>(dw);
        vp.Height = static_cast<float>(dh);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        d3dContext_->RSSetViewports(1, &vp);

        // The shared overlay shader keys, mirrors and fades by constant; a frame blit
        // wants none of that, so it runs as an opaque, unmirrored sprite.
        const recorder_core::OverlayPixelConstants pc = OpaqueQuadConstants();
        d3dContext_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &pc, 0, 0);

        d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        d3dContext_->PSSetShaderResources(0, 1, &srv);
        d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
        d3dContext_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
        d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext_->Draw(3, 0);
    };

    // Tracks the SRV + dimensions actually drawn this tick (vs. the back buffer
    // only having been cleared to black) — both for the snapshot readback below
    // (which blits this same source at full resolution, unscaled) and so a
    // snapshot taken before the first frame arrives reports "not ready yet"
    // instead of silently handing back a black image.
    ID3D11ShaderResourceView* drawnSrv = nullptr;
    uint32_t drawnW = 0;
    uint32_t drawnH = 0;
    if (drawPushed) {
        // An FP16 scRGB tap is drawn from its tone-mapped SDR surface; every other
        // format samples the raw private copy directly.
        drawnSrv = pushedSdrSRV_ ? pushedSdrSRV_.Get() : pushedLocalSRV_.Get();
        drawnW = pushedWidth_;
        drawnH = pushedHeight_;
        drawSource(drawnSrv, drawnW, drawnH);
    } else {
        std::lock_guard lock(frameMutex_);
        if (latestFrame_ && latestFrameSRV_ && srcWidth_ > 0 && srcHeight_ > 0) {
            drawnSrv = latestFrameSRV_.Get();
            drawnW = srcWidth_;
            drawnH = srcHeight_;
            drawSource(drawnSrv, drawnW, drawnH);
        }
    }

    // Composite the webcam PiP (+ chrome) over the main frame's content rect — but
    // NOT when drawing a pushed engine frame, which already has the PiP baked in
    // (drawing it again would double the overlay). A RAW pushed frame (idle
    // DXGI-hub source) carries no PiP, so the renderer keeps drawing its own.
    if (!drawPushed || pushed_.raw_source) {
        RenderWebcamOverlay(static_cast<int>(contentX), static_cast<int>(contentY), static_cast<int>(contentW),
                            static_cast<int>(contentH));
    }

    // A raw pushed frame carries no cursor either (Output Duplication composites
    // none) — draw the live sprite above the PiP, matching the recording
    // compositor's draw order.
    if (drawPushed && pushed_.raw_source) {
        RenderCursorSprite(static_cast<int>(contentX), static_cast<int>(contentY), static_cast<int>(contentW),
                           static_cast<int>(contentH));
    }

    if (drawnSrv != nullptr && drawnW != 0 && drawnH != 0 && !framePresented_.load(std::memory_order_relaxed)) {
        framePresented_.store(true, std::memory_order_release);
        if (firstFrameCallback_)
            firstFrameCallback_();
    }

    if (snapshotRequested_.load(std::memory_order_acquire)) {
        PerformSnapshotIfRequested(drawnSrv, drawnW, drawnH);
    }

    swapChain_->Present(1, 0);
}

void DxgiPreviewRenderer::RequestSnapshot(SnapshotCallback callback) {
    {
        std::lock_guard lock(snapshotCallbackMutex_);
        snapshotCallback_ = std::move(callback);
    }
    snapshotRequested_.store(true, std::memory_order_release);
}

void DxgiPreviewRenderer::SetFirstFramePresentedCallback(std::function<void()> cb) {
    firstFrameCallback_ = std::move(cb);
}

bool DxgiPreviewRenderer::HasPresentedFrame() const noexcept {
    return framePresented_.load(std::memory_order_acquire);
}

void DxgiPreviewRenderer::PerformSnapshotIfRequested(ID3D11ShaderResourceView* srv, uint32_t srcW, uint32_t srcH) {
    SnapshotCallback cb;
    {
        std::lock_guard lock(snapshotCallbackMutex_);
        cb = std::move(snapshotCallback_);
        snapshotCallback_ = nullptr;
    }
    snapshotRequested_.store(false, std::memory_order_release);
    if (!cb)
        return;

    if (srv == nullptr || srcW == 0 || srcH == 0) {
        cb(false, 0, 0, {}, "Preview has not rendered a frame yet");
        return;
    }

    // Lazily (re)allocate the full-resolution render target + staging texture to
    // match the source's current size (the source can change size across a
    // reconnect/resolution change).
    if (!snapshotRenderTex_ || snapshotStagingWidth_ != srcW || snapshotStagingHeight_ != srcH) {
        D3D11_TEXTURE2D_DESC rd{};
        rd.Width = srcW;
        rd.Height = srcH;
        rd.MipLevels = 1;
        rd.ArraySize = 1;
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc = {1, 0};
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = D3D11_BIND_RENDER_TARGET;
        snapshotRenderTex_.Reset();
        snapshotRenderRTV_.Reset();
        HRESULT rhr = d3dDevice_->CreateTexture2D(&rd, nullptr, snapshotRenderTex_.GetAddressOf());
        if (SUCCEEDED(rhr)) {
            rhr = d3dDevice_->CreateRenderTargetView(snapshotRenderTex_.Get(), nullptr,
                                                     snapshotRenderRTV_.GetAddressOf());
        }
        if (FAILED(rhr)) {
            char errbuf[80];
            snprintf(errbuf, sizeof(errbuf), "snapshot render target failed 0x%08lX", static_cast<unsigned long>(rhr));
            snapshotRenderTex_.Reset();
            snapshotRenderRTV_.Reset();
            cb(false, 0, 0, {}, errbuf);
            return;
        }

        D3D11_TEXTURE2D_DESC sd = rd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags = 0;
        snapshotStagingTex_.Reset();
        const HRESULT shr = d3dDevice_->CreateTexture2D(&sd, nullptr, snapshotStagingTex_.GetAddressOf());
        if (FAILED(shr)) {
            char errbuf[80];
            snprintf(errbuf, sizeof(errbuf), "snapshot staging tex failed 0x%08lX", static_cast<unsigned long>(shr));
            cb(false, 0, 0, {}, errbuf);
            return;
        }
        snapshotStagingWidth_ = srcW;
        snapshotStagingHeight_ = srcH;
    }

    // Blit the source SRV into the full-resolution render target — same shader
    // and opaque-quad constants as the on-screen draw, but covering the WHOLE
    // target (no letterbox/pillarbox, no downscale to the preview widget's
    // size): this is meant to match what the encoder sees, not the small
    // on-screen preview.
    d3dContext_->OMSetRenderTargets(1, snapshotRenderRTV_.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(srcW);
    vp.Height = static_cast<float>(srcH);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    d3dContext_->RSSetViewports(1, &vp);
    const recorder_core::OverlayPixelConstants pc = OpaqueQuadConstants();
    d3dContext_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &pc, 0, 0);
    d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShaderResources(0, 1, &srv);
    d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
    d3dContext_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dContext_->Draw(3, 0);

    d3dContext_->CopyResource(snapshotStagingTex_.Get(), snapshotRenderTex_.Get());

    // Map for CPU read (synchronization point — stalls until the GPU copy above
    // completes; typically <1 ms, same as the engine's own snapshot readback).
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mhr = d3dContext_->Map(snapshotStagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mhr)) {
        char errbuf[80];
        snprintf(errbuf, sizeof(errbuf), "snapshot map failed 0x%08lX", static_cast<unsigned long>(mhr));
        cb(false, 0, 0, {}, errbuf);
        return;
    }

    // RowPitch can exceed width*4 due to GPU row alignment — copy row by row
    // into a tightly packed buffer rather than a single bulk memcpy.
    std::vector<uint8_t> bgra(static_cast<size_t>(srcW) * srcH * 4);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(srcW) * 4;
    for (uint32_t y = 0; y < srcH; ++y) {
        std::memcpy(bgra.data() + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * mapped.RowPitch,
                    rowBytes);
    }
    d3dContext_->Unmap(snapshotStagingTex_.Get(), 0);

    cb(true, srcW, srcH, std::move(bgra), {});
}

void DxgiPreviewRenderer::RenderThreadProc(const recorder_core::CaptureTarget& target, uint32_t frame_interval_ms,
                                           std::stop_token stop_token) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInited = SUCCEEDED(coHr) || coHr == RPC_E_CHANGED_MODE;

    if (!InitD3D11()) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("render thread: D3D11 init failed"));
        return;
    }
    if (!InitSwapChain(initialWidth_, initialHeight_)) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("render thread: swap chain init failed"));
        return;
    }
    if (!InitShaders()) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("render thread: shader init failed"));
        return;
    }
    // Pushed-only mode never opens a capture of its own: the thread presents
    // whatever BeginPushedSource feeds it. Everything WGC below stays null.
    if (!pushedOnlyMode_) {
        if (!InitCaptureItem(target)) {
            if (comInited && SUCCEEDED(coHr))
                CoUninitialize();
            diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                         QStringLiteral("render thread: capture item init failed"));
            return;
        }
        if (!InitFramePool()) {
            if (comInited && SUCCEEDED(coHr))
                CoUninitialize();
            diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                         QStringLiteral("render thread: frame pool init failed"));
            return;
        }
    }

    diagnostics::AppLog::debug(QStringLiteral("dxgi-preview"),
                               QStringLiteral("render thread running interval=%1ms%2")
                                   .arg(frame_interval_ms)
                                   .arg(pushedOnlyMode_ ? QStringLiteral(" (pushed-only)") : QString{}));

    // Fixed-cadence frame pacing via a high-resolution waitable timer. The old
    // Sleep(1) busy-poll depended on the ~15 ms system timer granularity, so the
    // present interval wobbled (roughly 1-31 ms instead of a steady 16 ms). On a
    // VRR display that wobble makes the monitor continuously re-negotiate its
    // refresh rate, visible as subtle brightness pulsing — worse while a
    // translucent overlay (e.g. the notification hub) composites over the
    // preview. A precise per-frame wait keeps the cadence steady without raising
    // the global timer resolution.
    HANDLE frameTimer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER nextDeadline{};
    QueryPerformanceCounter(&nextDeadline);
    const LONGLONG intervalTicks = static_cast<LONGLONG>(frame_interval_ms) * qpcFreq.QuadPart / 1000;

    // Pushed mode keeps the loop alive even though the WGC graph (and its
    // source-lost signal) has been torn down.
    while (!stop_token.stop_requested() && (!sourceLost_.load() || pushed_.active)) {
        if (resizeRequested_.load()) {
            uint32_t rw = requestedWidth_.load();
            uint32_t rh = requestedHeight_.load();
            ResizeSwapChainInternal(rw, rh);
            resizeRequested_.store(false);
        }

        // The pushed source ended. In WGC mode: rebuild the preview's own capture
        // in place. In pushed-only mode there is nothing to revert TO — hold the
        // last presented image as the fallback frame and wait for the feeder's
        // next BeginPushedSource. Handled before the begin-request so a stale
        // revert never runs after a fresh handoff (BeginPushedSource clears the
        // end flag anyway).
        if (pushedEndRequested_.exchange(false, std::memory_order_acq_rel)) {
            if (pushedOnlyMode_) {
                if (pushedLocalTex_ && pushedWidth_ > 0 && pushedHeight_ > 0) {
                    std::lock_guard lock(frameMutex_);
                    latestFrame_ = pushedSdrTex_ ? pushedSdrTex_ : pushedLocalTex_;
                    latestFrameSRV_ = pushedSdrSRV_ ? pushedSdrSRV_ : pushedLocalSRV_;
                    srcWidth_ = pushedWidth_;
                    srcHeight_ = pushedHeight_;
                }
                ReleasePushedResources();
            } else {
                RevertToWgcCapture(target);
            }
        }

        if (pushedRequested_.load(std::memory_order_acquire)) {
            AdoptPendingPushedSource();
            if (pushed_.ShouldStopWgcGraph()) {
                // The engine is now the source of truth — stop the second capture.
                StopCaptureGraph();
                pushed_.OnWgcGraphStopped();
            }
        }

        if (pushed_.PollsWgc())
            PollAndProcessFrames();
        else
            ConsumePushedFrame();
        RenderFrame();

        nextDeadline.QuadPart += intervalTicks;
        LARGE_INTEGER nowTick{};
        QueryPerformanceCounter(&nowTick);
        const LONGLONG remaining = nextDeadline.QuadPart - nowTick.QuadPart;
        if (remaining <= 0) {
            // Render took longer than one interval — resync rather than bursting
            // catch-up frames.
            nextDeadline = nowTick;
            continue;
        }
        if (frameTimer) {
            LARGE_INTEGER due{};
            due.QuadPart = -(remaining * 10000000LL / qpcFreq.QuadPart); // relative, 100 ns units
            if (SetWaitableTimer(frameTimer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(frameTimer, frame_interval_ms + 100);
            else
                Sleep(static_cast<DWORD>(remaining * 1000 / qpcFreq.QuadPart));
        } else {
            Sleep(static_cast<DWORD>(remaining * 1000 / qpcFreq.QuadPart));
        }
    }

    if (frameTimer)
        CloseHandle(frameTimer);

    if (sourceLost_.load())
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"), QStringLiteral("capture source lost"));

    CleanupCapture();
    if (comInited && SUCCEEDED(coHr))
        CoUninitialize();

    diagnostics::AppLog::debug(QStringLiteral("dxgi-preview"), QStringLiteral("render thread stopped"));
}

} // namespace exosnap
