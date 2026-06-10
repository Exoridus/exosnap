#include "PreviewService.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <windows.h>

#include <d3d11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace exosnap {

PreviewService::~PreviewService() {
    Stop();
}

void PreviewService::SetFrameCallback(FrameCallback cb) {
    frame_callback_ = std::move(cb);
}

bool PreviewService::Start(const recorder_core::CaptureTarget& target) {
    Stop();
    running_.store(true);
    thread_ = std::jthread([this, target](std::stop_token st) {
        ThreadMain(target, std::move(st));
        running_.store(false);
    });
    return true;
}

void PreviewService::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    running_.store(false);
}

bool PreviewService::IsRunning() const noexcept {
    return running_.load();
}

void PreviewService::PostFrame(QImage frame) {
    if (!frame_callback_)
        return;
    auto cb = frame_callback_;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [cb = std::move(cb), frame = std::move(frame)]() mutable { cb(std::move(frame)); }, Qt::QueuedConnection);
}

void PreviewService::ThreadMain(recorder_core::CaptureTarget target, std::stop_token stop_token) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_inited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!com_inited)
        return;

    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, d3dDevice.put(), nullptr,
                               d3dContext.put());
        if (FAILED(hr) || !d3dDevice) {
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
            winrt::check_hresult(interop->CreateForMonitor(
                reinterpret_cast<HMONITOR>(target.native_id),
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(
                reinterpret_cast<HWND>(target.native_id),
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        }
    } catch (...) {
        if (com_inited)
            CoUninitialize();
        return;
    }

    const auto capSz = item.Size();
    if (capSz.Width <= 0 || capSz.Height <= 0) {
        if (com_inited)
            CoUninitialize();
        return;
    }

    const uint32_t width = static_cast<uint32_t>(capSz.Width);
    const uint32_t height = static_cast<uint32_t>(capSz.Height);

    winrt::com_ptr<ID3D11Texture2D> stagingTex;
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = d3dDevice->CreateTexture2D(&desc, nullptr, stagingTex.put());
        if (FAILED(hr)) {
            if (com_inited)
                CoUninitialize();
            return;
        }
    }

    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{nullptr};
    bool sourceLost = false;

    try {
        winrt::com_ptr<IDXGIDevice> dxgiDev = d3dDevice.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
        auto d3dWinRTDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
            d3dWinRTDev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, capSz);

        captureSession = framePool.CreateCaptureSession(item);
        captureSession.IsBorderRequired(false);
        captureSession.StartCapture();

        item.Closed([&sourceLost](const auto&, const auto&) { sourceLost = true; });
    } catch (...) {
        if (com_inited)
            CoUninitialize();
        return;
    }

    constexpr DWORD kFrameIntervalMs = 33; // ~30 fps
    DWORD lastFrameMs = 0;

    while (!stop_token.stop_requested() && !sourceLost) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const DWORD now = GetTickCount();
        if (now - lastFrameMs < kFrameIntervalMs) {
            Sleep(2);
            continue;
        }

        try {
            auto frame = framePool.TryGetNextFrame();
            if (frame == nullptr) {
                Sleep(2);
                continue;
            }

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> frameTex;
            if (FAILED(access->GetInterface(IID_PPV_ARGS(frameTex.put())))) {
                Sleep(2);
                continue;
            }

            D3D11_TEXTURE2D_DESC frameDesc{};
            frameTex->GetDesc(&frameDesc);

            if (frameDesc.Width == width && frameDesc.Height == height) {
                d3dContext->CopyResource(stagingTex.get(), frameTex.get());

                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = d3dContext->Map(stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    // Wrap the mapped buffer without copying, then deep-copy before unmap.
                    // DXGI_FORMAT_B8G8R8A8_UNORM on LE x86 = [B,G,R,A] bytes = Format_ARGB32 storage.
                    QImage view(static_cast<const uchar*>(mapped.pData), static_cast<int>(width),
                                static_cast<int>(height), static_cast<int>(mapped.RowPitch), QImage::Format_ARGB32);
                    QImage owned = view.copy();
                    d3dContext->Unmap(stagingTex.get(), 0);
                    PostFrame(std::move(owned));
                    lastFrameMs = now;
                }
            }
        } catch (...) {
            Sleep(2);
        }
    }

    if (captureSession != nullptr)
        captureSession.Close();
    if (framePool != nullptr)
        framePool.Close();
    if (com_inited)
        CoUninitialize();
}

} // namespace exosnap
