#include "DxgiPreviewRenderer.h"

#include "../diagnostics/AppLog.h"

#include <recorder_core/webcam_placement.h>

#include <cstring>

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

void DxgiPreviewRenderer::SetWebcamOverlayState(bool enabled, bool selected, float nx, float ny, float nw, float nh,
                                                bool mirror) {
    std::lock_guard lock(overlayMutex_);
    overlayEnabled_ = enabled;
    overlaySelected_ = selected;
    overlayNx_ = nx;
    overlayNy_ = ny;
    overlayNw_ = nw;
    overlayNh_ = nh;
    if (overlayMirror_ != mirror) {
        overlayMirror_ = mirror;
        overlayDirty_ = true; // re-upload with the new flip
    }
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

    const int rowBytes = overlayW_ * 4;
    if (overlayMirror_) {
        // Real horizontal flip (no vertical flip) — matches the recording compositor.
        overlayScratch_.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(overlayH_));
        for (int y = 0; y < overlayH_; ++y) {
            const uint8_t* src = overlayBgra_.data() + static_cast<size_t>(y) * rowBytes;
            uint8_t* dst = overlayScratch_.data() + static_cast<size_t>(y) * rowBytes;
            for (int x = 0; x < overlayW_; ++x) {
                const uint8_t* sp = src + static_cast<size_t>(overlayW_ - 1 - x) * 4;
                uint8_t* dp = dst + static_cast<size_t>(x) * 4;
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = sp[3];
            }
        }
        d3dContext_->UpdateSubresource(overlayTex_.Get(), 0, nullptr, overlayScratch_.data(),
                                       static_cast<UINT>(rowBytes), 0);
    } else {
        d3dContext_->UpdateSubresource(overlayTex_.Get(), 0, nullptr, overlayBgra_.data(), static_cast<UINT>(rowBytes),
                                       0);
    }
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
    placement.mirror = overlayMirror_; // mirror already baked into the texture
    const recorder_core::WebcamPixelRect r =
        recorder_core::MapWebcamPlacementToContent(placement, contentX, contentY, contentW, contentH);
    if (!r.IsValid())
        return;

    d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
    d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

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

    // PiP video (stretched to the rect — same as the recording compositor).
    drawQuad(overlaySRV_.Get(), r.x, r.y, r.w, r.h);

    // Edit chrome (border + corner handles) only while selected.
    if (overlaySelected_) {
        EnsureChromeTexture();
        if (chromeSRV_) {
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

    {
        std::lock_guard lock(frameMutex_);
        if (latestFrame_ && srcWidth_ > 0 && srcHeight_ > 0) {
            LONG dx = 0, dy = 0, dw = static_cast<LONG>(bbDesc.Width), dh = static_cast<LONG>(bbDesc.Height);
            ComputeContainFitRect(static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height),
                                  static_cast<LONG>(srcWidth_), static_cast<LONG>(srcHeight_), dx, dy, dw, dh);

            if (dw > 0 && dh > 0) {
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

                d3dContext_->VSSetShader(vertexShader_.Get(), nullptr, 0);
                d3dContext_->PSSetShader(pixelShader_.Get(), nullptr, 0);
                d3dContext_->PSSetShaderResources(0, 1, latestFrameSRV_.GetAddressOf());
                d3dContext_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());
                d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                d3dContext_->Draw(3, 0);
            }
        }
    }

    // Composite the webcam PiP (+ chrome) over the main frame's content rect.
    RenderWebcamOverlay(static_cast<int>(contentX), static_cast<int>(contentY), static_cast<int>(contentW),
                        static_cast<int>(contentH));

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
