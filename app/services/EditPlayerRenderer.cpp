// app/services/EditPlayerRenderer.cpp -- real native child-HWND GPU render
// host for the editor player (2026-08-03 editor-playback-gpu-render design,
// Task 4). Structural shape (device/swap-chain init, viewport-scoped
// full-screen-triangle "sprite" draws through the shared overlay shader)
// mirrors DxgiPreviewRenderer, without its capture graph / webcam PiP /
// cursor sprite / snapshot machinery -- see the header comment for the
// threading-model decision.
#include "EditPlayerRenderer.h"

#include "../diagnostics/AppLog.h"
#include "PreviewHelpers.h" // ComputeContainFitRect

#include <recorder_core/overlay_shader.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stop_token>

namespace exosnap {

namespace {

constexpr const wchar_t* kChildWindowClass = L"ExoSnapEditPlayerChild";

// Panel background the old QPainter path used (EditPlayerSurface.cpp's
// bg_grad, averaged) -- the swap chain clears to this, and the placeholder
// sprite is painted over the same colour, so neither shows a seam.
constexpr float kPanelClearColor[4] = {0.055f, 0.051f, 0.043f, 1.0f}; // ~#0e0d0b

} // namespace

EditPlayerRenderer::EditPlayerRenderer() = default;

EditPlayerRenderer::~EditPlayerRenderer() {
    Shutdown();
}

bool EditPlayerRenderer::Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight) {
    if (initialized_.load())
        return true;

    if (!parentHwnd) {
        diagnostics::AppLog::warning(QStringLiteral("edit-player"),
                                     QStringLiteral("initialize failed: null parent HWND"));
        return false;
    }
    parentHwnd_ = parentHwnd;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kChildWindowClass;

    if (RegisterClassExW(&wc) == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            diagnostics::AppLog::warning(
                QStringLiteral("edit-player"),
                QStringLiteral("RegisterClassExW failed: %1").arg(static_cast<unsigned long>(err)));
            return false;
        }
    }

    childHwnd_ = CreateWindowExW(0, kChildWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                 static_cast<int>(hwndWidth), static_cast<int>(hwndHeight), parentHwnd_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (!childHwnd_) {
        diagnostics::AppLog::warning(
            QStringLiteral("edit-player"),
            QStringLiteral("CreateWindowExW failed: %1").arg(static_cast<unsigned long>(GetLastError())));
        return false;
    }
    SetWindowPos(childHwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // The render thread brings up the D3D11 device/swap chain/shaders and
    // reports success back through this promise -- Initialize() blocks on it
    // so its own return value is an immediate, honest answer (see the header
    // comment's threading-model rationale).
    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();
    hwndWidth = std::max<uint32_t>(hwndWidth, 1);
    hwndHeight = std::max<uint32_t>(hwndHeight, 1);

    renderThread_ =
        std::jthread([this, promise = std::move(initPromise), hwndWidth, hwndHeight](std::stop_token st) mutable {
            RenderThreadProc(hwndWidth, hwndHeight, std::move(promise), st);
        });

    const bool ok = initFuture.get();
    initialized_.store(ok, std::memory_order_release);
    if (!ok) {
        if (renderThread_.joinable())
            renderThread_.join();
        DestroyWindow(childHwnd_);
        childHwnd_ = nullptr;
        return false;
    }
    return true;
}

void EditPlayerRenderer::Resize(uint32_t hwndWidth, uint32_t hwndHeight) {
    if (!childHwnd_)
        return;
    hwndWidth = std::max<uint32_t>(hwndWidth, 1);
    hwndHeight = std::max<uint32_t>(hwndHeight, 1);

    SetWindowPos(childHwnd_, nullptr, 0, 0, static_cast<int>(hwndWidth), static_cast<int>(hwndHeight),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingSwapWidth_ = hwndWidth;
        pendingSwapHeight_ = hwndHeight;
        resizePending_ = true;
    }
    stateCv_.notify_all();
}

void EditPlayerRenderer::PresentFrame(recorder_core::RawDecodedVideoFrame frame, float hdr_peak_scale) {
    if (!initialized_.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        // Overwrite: only the newest undrawn frame matters (see the header's
        // threading-model comment) -- an older one that never got drawn is
        // simply superseded, not queued.
        pendingFrame_ = std::move(frame);
        pendingPeakScale_ = hdr_peak_scale;
    }
    stateCv_.notify_all();
}

void EditPlayerRenderer::ShowPlaceholder(const std::wstring& text) {
    if (!initialized_.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pendingPlaceholderText_ = text;
        pendingPlaceholderDirty_ = true;
        // A placeholder request supersedes any not-yet-drawn frame -- mirrors
        // EditPlayerSurface::clearFrame()'s old "drop the frame" contract.
        pendingFrame_.reset();
    }
    stateCv_.notify_all();
}

void EditPlayerRenderer::SetClockUs(int64_t media_time_us) noexcept {
    clockUs_.store(media_time_us, std::memory_order_relaxed);
}

bool EditPlayerRenderer::HasPresentedFrame() const noexcept {
    return hasPresentedFrame_.load(std::memory_order_acquire);
}

void EditPlayerRenderer::SetChildWindowVisible(bool visible) noexcept {
    if (!childHwnd_)
        return;
    ShowWindow(childHwnd_, visible ? SW_SHOWNA : SW_HIDE);
}

void EditPlayerRenderer::Shutdown() {
    if (renderThread_.joinable()) {
        renderThread_.request_stop();
        stateCv_.notify_all();
        renderThread_.join();
    }
    initialized_.store(false, std::memory_order_release);
    hasPresentedFrame_.store(false, std::memory_order_release);
    if (childHwnd_) {
        DestroyWindow(childHwnd_);
        childHwnd_ = nullptr;
    }
    parentHwnd_ = nullptr;
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

void EditPlayerRenderer::RenderThreadProc(uint32_t initWidth, uint32_t initHeight, std::promise<bool> initPromise,
                                          std::stop_token stop_token) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInited = SUCCEEDED(coHr) || coHr == RPC_E_CHANGED_MODE;

    bool ok = InitD3D11() && InitSwapChain(initWidth, initHeight) && InitShaders();
    if (ok) {
        std::string err;
        converter_ = std::make_unique<recorder_core::EditFrameGpuConverter>();
        if (!converter_->Init(d3dDevice_.Get(), d3dContext_.Get(), err)) {
            diagnostics::AppLog::warning(
                QStringLiteral("edit-player"),
                QStringLiteral("EditFrameGpuConverter::Init failed: %1").arg(QString::fromStdString(err)));
            ok = false;
        }
    }

    initPromise.set_value(ok);
    if (!ok) {
        ReleaseGpuResources();
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        return;
    }

    // Wakes a wait() parked below the instant a stop is requested, even if no
    // other hand-off ever arrives again.
    std::stop_callback wakeOnStop(stop_token, [this] { stateCv_.notify_all(); });

    while (!stop_token.stop_requested()) {
        std::unique_lock<std::mutex> lock(stateMutex_);
        stateCv_.wait(lock, [this, &stop_token] {
            return stop_token.stop_requested() || pendingFrame_.has_value() || pendingPlaceholderDirty_ ||
                   resizePending_;
        });
        if (stop_token.stop_requested())
            break;

        std::optional<recorder_core::RawDecodedVideoFrame> frame = std::move(pendingFrame_);
        pendingFrame_.reset();
        const float peakScale = pendingPeakScale_;

        const bool placeholderDirty = pendingPlaceholderDirty_;
        pendingPlaceholderDirty_ = false;
        const std::wstring placeholderText = pendingPlaceholderText_;

        const bool doResize = resizePending_;
        resizePending_ = false;
        const uint32_t resizeW = pendingSwapWidth_;
        const uint32_t resizeH = pendingSwapHeight_;
        lock.unlock();

        if (doResize)
            ResizeSwapChainInternal(resizeW, resizeH);

        if (placeholderDirty) {
            showFrame_ = false;
            lastPlaceholderText_ = placeholderText;
        }

        bool needsRedraw = doResize || placeholderDirty;

        if (frame.has_value()) {
            const int64_t clock = clockUs_.load(std::memory_order_relaxed);
            if (clock >= 0 && frame->pts_us < clock) {
                // Stale: the playback clock already passed this frame's own
                // timestamp -- dropped without any GPU work (upload, convert,
                // Present), per the spec's "Presentation cadence" gate.
            } else {
                UploadAndConvert(*frame, peakScale);
                needsRedraw = true;
            }
        }

        // A resize while showing the placeholder needs the GDI sprite
        // regenerated at the new swap-chain size (it always covers the whole
        // client area -- see RegeneratePlaceholderTexture's comment).
        if (!showFrame_ && (placeholderDirty || doResize))
            RegeneratePlaceholderTexture(lastPlaceholderText_);

        if (needsRedraw)
            DrawAndPresent();
    }

    ReleaseGpuResources();
    if (comInited && SUCCEEDED(coHr))
        CoUninitialize();
}

bool EditPlayerRenderer::InitD3D11() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                         static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                         d3dDevice_.GetAddressOf(), nullptr, d3dContext_.GetAddressOf());
    if (FAILED(hr)) {
        diagnostics::AppLog::warning(
            QStringLiteral("edit-player"),
            QStringLiteral("D3D11CreateDevice failed: 0x%1").arg(static_cast<unsigned long>(hr), 8, 16));
        return false;
    }
    return true;
}

bool EditPlayerRenderer::InitSwapChain(uint32_t width, uint32_t height) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3dDevice_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    if (FAILED(dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf())))
        return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    const HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice_.Get(), childHwnd_, &scDesc, nullptr, nullptr,
                                                           swapChain_.GetAddressOf());
    if (FAILED(hr)) {
        diagnostics::AppLog::warning(
            QStringLiteral("edit-player"),
            QStringLiteral("CreateSwapChainForHwnd failed: 0x%1").arg(static_cast<unsigned long>(hr), 8, 16));
        return false;
    }
    return true;
}

bool EditPlayerRenderer::InitShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr =
        D3DCompile(recorder_core::kOverlayVertexShaderSrc, strlen(recorder_core::kOverlayVertexShaderSrc), "vs_main",
                   nullptr, nullptr, "main", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        diagnostics::AppLog::warning(QStringLiteral("edit-player"), QStringLiteral("vertex shader compile failed"));
        return false;
    }

    hr = D3DCompile(recorder_core::kOverlayPixelShaderSrc, strlen(recorder_core::kOverlayPixelShaderSrc), "ps_main",
                    nullptr, nullptr, "main", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        diagnostics::AppLog::warning(QStringLiteral("edit-player"), QStringLiteral("pixel shader compile failed"));
        return false;
    }

    if (FAILED(d3dDevice_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                              vertexShader_.GetAddressOf())))
        return false;
    if (FAILED(d3dDevice_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                             pixelShader_.GetAddressOf())))
        return false;

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(d3dDevice_->CreateSamplerState(&sampDesc, samplerState_.GetAddressOf())))
        return false;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(recorder_core::OverlayPixelConstants);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d3dDevice_->CreateBuffer(&cbDesc, nullptr, constantBuffer_.GetAddressOf())))
        return false;

    return true;
}

void EditPlayerRenderer::ResizeSwapChainInternal(uint32_t width, uint32_t height) {
    if (!swapChain_ || width < 1 || height < 1)
        return;

    d3dContext_->OMSetRenderTargets(0, nullptr, nullptr);
    d3dContext_->Flush();
    swapChain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
}

bool EditPlayerRenderer::EnsureConvertedTarget(uint32_t width, uint32_t height) {
    if (convertedTex_ && convertedW_ == width && convertedH_ == height)
        return true;

    convertedTex_.Reset();
    convertedSRV_.Reset();

    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3dDevice_->CreateTexture2D(&d, nullptr, convertedTex_.GetAddressOf())))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = d.Format;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MipLevels = 1;
    if (FAILED(d3dDevice_->CreateShaderResourceView(convertedTex_.Get(), &s, convertedSRV_.GetAddressOf()))) {
        convertedTex_.Reset();
        return false;
    }

    convertedW_ = width;
    convertedH_ = height;
    return true;
}

void EditPlayerRenderer::UploadAndConvert(const recorder_core::RawDecodedVideoFrame& frame, float hdr_peak_scale) {
    if (!converter_ || frame.width == 0 || frame.height == 0)
        return;
    if (!EnsureConvertedTarget(frame.width, frame.height))
        return;

    std::string err;
    if (!converter_->Convert(frame, convertedTex_.Get(), hdr_peak_scale, err)) {
        diagnostics::AppLog::warning(
            QStringLiteral("edit-player"),
            QStringLiteral("EditFrameGpuConverter::Convert failed: %1").arg(QString::fromStdString(err)));
        return;
    }

    frameSrcW_ = frame.width;
    frameSrcH_ = frame.height;
    showFrame_ = true;
}

void EditPlayerRenderer::RegeneratePlaceholderTexture(const std::wstring& text) {
    // The sprite always covers the whole swap-chain client area (see the
    // header comment), so its size tracks the CURRENT back buffer.
    if (!swapChain_)
        return;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(swapChain_->GetDesc1(&desc)))
        return;
    const uint32_t w = std::max<UINT>(desc.Width, 1);
    const uint32_t h = std::max<UINT>(desc.Height, 1);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(w);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!memDc)
        return;
    HBITMAP dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(memDc);
        return;
    }
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDc, dib));

    RECT full{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    HBRUSH bg = CreateSolidBrush(RGB(0x0e, 0x0d, 0x0b));
    FillRect(memDc, &full, bg);
    DeleteObject(bg);

    if (!text.empty()) {
        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, RGB(255, 255, 255));
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT oldFont = static_cast<HFONT>(SelectObject(memDc, font));
        RECT textRect = full;
        InflateRect(&textRect, -32, -32);
        DrawTextW(memDc, text.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOCLIP);
        SelectObject(memDc, oldFont);
        DeleteObject(font);
    }

    // (Re)create the target texture only when the size actually changed.
    if (!placeholderTex_ || placeholderTexW_ != w || placeholderTexH_ != h) {
        placeholderTex_.Reset();
        placeholderSRV_.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(d3dDevice_->CreateTexture2D(&d, nullptr, placeholderTex_.GetAddressOf()))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = d.Format;
            s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            s.Texture2D.MipLevels = 1;
            if (FAILED(
                    d3dDevice_->CreateShaderResourceView(placeholderTex_.Get(), &s, placeholderSRV_.GetAddressOf()))) {
                placeholderTex_.Reset();
            } else {
                placeholderTexW_ = w;
                placeholderTexH_ = h;
            }
        } else {
            placeholderTex_.Reset();
        }
    }
    if (placeholderTex_)
        d3dContext_->UpdateSubresource(placeholderTex_.Get(), 0, nullptr, bits, static_cast<UINT>(w) * 4u, 0);

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
}

void EditPlayerRenderer::DrawSprite(ID3D11ShaderResourceView* srv, long x, long y, long w, long h) {
    if (w <= 0 || h <= 0 || !srv)
        return;

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(x);
    vp.TopLeftY = static_cast<float>(y);
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    d3dContext_->RSSetViewports(1, &vp);

    // Opaque, unmirrored blit: neither a video frame nor the placeholder
    // sprite is keyed, mirrored or partially faded.
    const recorder_core::OverlayPixelConstants pc =
        recorder_core::MakeOverlayPixelConstants(recorder_core::ChromaKeyParams{}, /*mirror=*/false,
                                                 /*force_opaque=*/true, /*opacity=*/1.0f, /*hdr_linear=*/false,
                                                 /*ref_white_scale=*/1.0f);
    d3dContext_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &pc, 0, 0);

    d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShaderResources(0, 1, &srv);
    d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
    d3dContext_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dContext_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    d3dContext_->PSSetShaderResources(0, 1, &nullSrv);
}

void EditPlayerRenderer::DrawAndPresent() {
    if (!swapChain_ || !d3dContext_)
        return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(d3dDevice_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv.GetAddressOf())))
        return;

    d3dContext_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    d3dContext_->ClearRenderTargetView(rtv.Get(), kPanelClearColor);

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    bool drewFrame = false;
    if (showFrame_ && convertedSRV_ && frameSrcW_ > 0 && frameSrcH_ > 0) {
        LONG dx = 0, dy = 0, dw = static_cast<LONG>(bbDesc.Width), dh = static_cast<LONG>(bbDesc.Height);
        exosnap::ComputeContainFitRect(static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height),
                                       static_cast<LONG>(frameSrcW_), static_cast<LONG>(frameSrcH_), dx, dy, dw, dh);
        DrawSprite(convertedSRV_.Get(), dx, dy, dw, dh);
        drewFrame = true;
    } else if (placeholderSRV_) {
        DrawSprite(placeholderSRV_.Get(), 0, 0, static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height));
    }

    if (drewFrame && !hasPresentedFrame_.load(std::memory_order_relaxed))
        hasPresentedFrame_.store(true, std::memory_order_release);

    swapChain_->Present(1, 0);
}

void EditPlayerRenderer::ReleaseGpuResources() {
    convertedTex_.Reset();
    convertedSRV_.Reset();
    convertedW_ = 0;
    convertedH_ = 0;
    placeholderTex_.Reset();
    placeholderSRV_.Reset();
    placeholderTexW_ = 0;
    placeholderTexH_ = 0;
    constantBuffer_.Reset();
    samplerState_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    swapChain_.Reset();
    converter_.reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
}

} // namespace exosnap
