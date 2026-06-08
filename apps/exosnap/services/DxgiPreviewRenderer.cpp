#include "DxgiPreviewRenderer.h"

#include "../diagnostics/AppLog.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>

namespace exosnap {
namespace {

constexpr const wchar_t* kChildWindowClass = L"ExoSnapDxgiPreviewChild";

const char* kVertexShaderSrc = R"(
struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.texcoord = float2((id << 1) & 2, id & 2);
    output.position = float4(output.texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
)";

const char* kPixelShaderSrc = R"(
Texture2D frameTex : register(t0);
SamplerState frameSamp : register(s0);

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    return frameTex.Sample(frameSamp, texcoord);
}
)";

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
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] initialize failed: null parent HWND"));
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
            diagnostics::AppLog(
                QStringLiteral("[dxgi-preview] RegisterClassExW failed: %1").arg(static_cast<unsigned long>(err)));
            return false;
        }
    }

    childHwnd_ = CreateWindowExW(0, kChildWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                 static_cast<int>(hwndWidth), static_cast<int>(hwndHeight), parentHwnd_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);

    if (!childHwnd_) {
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] CreateWindowExW failed: %1")
                                .arg(static_cast<unsigned long>(GetLastError())));
        return false;
    }

    SetWindowPos(childHwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    initialized_.store(true);
    diagnostics::AppLog(QStringLiteral("[dxgi-preview] initialized OK parent=0x%1 child=0x%2 hwnd=%3x%4 swap=%5x%6")
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

void DxgiPreviewRenderer::Shutdown() {
    StopCapture();

    if (childHwnd_) {
        DestroyWindow(childHwnd_);
        childHwnd_ = nullptr;
    }

    initialized_.store(false);
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
        diagnostics::AppLog(
            QStringLiteral("[dxgi-preview] D3D11CreateDevice failed: 0x%1").arg(static_cast<unsigned long>(hr), 8, 16));
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
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] CreateSwapChainForHwnd failed: 0x%1")
                                .arg(static_cast<unsigned long>(hr), 8, 16));
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

    HRESULT hr = D3DCompile(kVertexShaderSrc, strlen(kVertexShaderSrc), "vs_main", nullptr, nullptr, "main", "vs_5_0",
                            0, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob)
            diagnostics::AppLog(QStringLiteral("[dxgi-preview] vertex shader compile failed"));
        return false;
    }

    hr = D3DCompile(kPixelShaderSrc, strlen(kPixelShaderSrc), "ps_main", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                    psBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob)
            diagnostics::AppLog(QStringLiteral("[dxgi-preview] pixel shader compile failed"));
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
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] create capture item failed"));
        return false;
    }

    if (!captureItem_) {
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] capture item is null after creation"));
        return false;
    }

    auto sz = captureItem_.Size();
    diagnostics::AppLog(
        QStringLiteral("[dxgi-preview] capture item created OK size=%1x%2").arg(sz.Width).arg(sz.Height));
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
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] InitFramePool failed"));
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

    {
        std::lock_guard lock(frameMutex_);
        if (latestFrame_ && srcWidth_ > 0 && srcHeight_ > 0) {
            LONG dx = 0, dy = 0, dw = static_cast<LONG>(bbDesc.Width), dh = static_cast<LONG>(bbDesc.Height);
            ComputeContainFitRect(static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height),
                                  static_cast<LONG>(srcWidth_), static_cast<LONG>(srcHeight_), dx, dy, dw, dh);

            if (dw > 0 && dh > 0) {
                D3D11_VIEWPORT vp{};
                vp.TopLeftX = static_cast<float>(dx);
                vp.TopLeftY = static_cast<float>(dy);
                vp.Width = static_cast<float>(dw);
                vp.Height = static_cast<float>(dh);
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;
                d3dContext_->RSSetViewports(1, &vp);

                d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
                d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
                d3dContext_->PSSetShaderResources(0, 1, latestFrameSRV_.GetAddressOf());
                d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
                d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                d3dContext_->Draw(3, 0);
            }
        }
    }

    swapChain_->Present(1, 0);
}

void DxgiPreviewRenderer::RenderThreadProc(const recorder_core::CaptureTarget& target, uint32_t frame_interval_ms,
                                           std::stop_token stop_token) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInited = SUCCEEDED(coHr) || coHr == RPC_E_CHANGED_MODE;

    if (!InitD3D11()) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread: D3D11 init failed"));
        return;
    }
    if (!InitSwapChain(initialWidth_, initialHeight_)) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread: swap chain init failed"));
        return;
    }
    if (!InitShaders()) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread: shader init failed"));
        return;
    }
    if (!InitCaptureItem(target)) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread: capture item init failed"));
        return;
    }
    if (!InitFramePool()) {
        if (comInited && SUCCEEDED(coHr))
            CoUninitialize();
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread: frame pool init failed"));
        return;
    }

    diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread running interval=%1ms").arg(frame_interval_ms));

    ULONGLONG lastFrameMs = 0;

    while (!stop_token.stop_requested() && !sourceLost_.load()) {
        if (resizeRequested_.load()) {
            uint32_t rw = requestedWidth_.load();
            uint32_t rh = requestedHeight_.load();
            ResizeSwapChainInternal(rw, rh);
            resizeRequested_.store(false);
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastFrameMs < frame_interval_ms) {
            Sleep(1);
            continue;
        }

        PollAndProcessFrames();
        RenderFrame();
        lastFrameMs = now;
    }

    if (sourceLost_.load())
        diagnostics::AppLog(QStringLiteral("[dxgi-preview] capture source lost"));

    CleanupCapture();
    if (comInited && SUCCEEDED(coHr))
        CoUninitialize();

    diagnostics::AppLog(QStringLiteral("[dxgi-preview] render thread stopped"));
}

} // namespace exosnap
