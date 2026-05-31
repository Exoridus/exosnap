#include "ThumbnailCapture.h"

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
namespace {

QImage downscaleThumbnail(const QImage& source, QSize desired) {
    if (source.isNull() || desired.width() <= 0 || desired.height() <= 0)
        return source;

    const int src_w = source.width();
    const int src_h = source.height();
    if (src_w <= 0 || src_h <= 0)
        return source;

    if (src_w <= desired.width() && src_h <= desired.height())
        return source;

    const auto scale =
        std::min(static_cast<qreal>(desired.width()) / src_w, static_cast<qreal>(desired.height()) / src_h);
    const int dst_w = std::max(1, static_cast<int>(src_w * scale));
    const int dst_h = std::max(1, static_cast<int>(src_h * scale));

    return source.scaled(dst_w, dst_h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage captureOneFrame(uintptr_t native_id, bool is_monitor, QSize desired_size) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_inited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!com_inited)
        return {};

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
            return {};
        }
    }

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        if (is_monitor) {
            winrt::check_hresult(interop->CreateForMonitor(
                reinterpret_cast<HMONITOR>(native_id),
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(
                reinterpret_cast<HWND>(native_id),
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        }
    } catch (...) {
        if (com_inited)
            CoUninitialize();
        return {};
    }

    const auto capSz = item.Size();
    if (capSz.Width <= 0 || capSz.Height <= 0) {
        if (com_inited)
            CoUninitialize();
        return {};
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
            return {};
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
        return {};
    }

    QImage result;

    const DWORD startMs = GetTickCount();
    constexpr DWORD kTotalTimeoutMs = 2000;
    int frameCount = 0;

    while (frameCount < 2 && (GetTickCount() - startMs) < kTotalTimeoutMs && !sourceLost) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        try {
            auto frame = framePool.TryGetNextFrame();
            if (frame == nullptr) {
                Sleep(10);
                continue;
            }

            auto surface = frame.Surface();
            auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> frameTex;
            if (FAILED(access->GetInterface(IID_PPV_ARGS(frameTex.put())))) {
                Sleep(10);
                continue;
            }

            D3D11_TEXTURE2D_DESC frameDesc{};
            frameTex->GetDesc(&frameDesc);

            if (frameDesc.Width == width && frameDesc.Height == height) {
                d3dContext->CopyResource(stagingTex.get(), frameTex.get());

                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = d3dContext->Map(stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    QImage view(static_cast<const uchar*>(mapped.pData), static_cast<int>(width),
                                static_cast<int>(height), static_cast<int>(mapped.RowPitch), QImage::Format_ARGB32);
                    result = view.copy();
                    d3dContext->Unmap(stagingTex.get(), 0);
                    ++frameCount;
                    if (frameCount >= 2 || !result.isNull())
                        break;
                }
            }
        } catch (...) {
            break;
        }
    }

    if (captureSession != nullptr)
        captureSession.Close();
    if (framePool != nullptr)
        framePool.Close();
    if (com_inited)
        CoUninitialize();

    if (!result.isNull() && desired_size.width() > 0 && desired_size.height() > 0) {
        result = downscaleThumbnail(result, desired_size);
    }

    return result;
}

} // namespace

ThumbnailCapture::ThumbnailCapture(QObject* parent) : QObject(parent) {
    worker_ = std::jthread([this](std::stop_token st) {
        while (!st.stop_requested()) {
            Request req;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return !pending_.empty(); });
                if (pending_.empty())
                    continue;
                req = pending_.back();
                pending_.pop_back();
            }

            QImage result;
            try {
                result = captureOneFrame(req.native_id, req.is_monitor, req.desired_size);
            } catch (...) {
                result = {};
            }

            if (cancelled_.load())
                continue;

            if (result.isNull()) {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(), [this, idx = req.target_index]() { emit thumbnailFailed(idx); },
                    Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [this, idx = req.target_index, img = std::move(result)]() mutable {
                        emit thumbnailReady(idx, std::move(img));
                    },
                    Qt::QueuedConnection);
            }
        }
    });
}

ThumbnailCapture::~ThumbnailCapture() {
    cancelAll();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void ThumbnailCapture::requestMonitorThumbnail(int target_index, uintptr_t hmonitor, QSize desired_size) {
    queueCapture(target_index, hmonitor, true, desired_size);
}

void ThumbnailCapture::requestWindowThumbnail(int target_index, uintptr_t hwnd, QSize desired_size) {
    queueCapture(target_index, hwnd, false, desired_size);
}

void ThumbnailCapture::cancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
    }
    cv_.notify_one();
}

void ThumbnailCapture::queueCapture(int target_index, uintptr_t native_id, bool is_monitor, QSize desired_size) {
    cancelled_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Request req;
        req.target_index = target_index;
        req.native_id = native_id;
        req.is_monitor = is_monitor;
        req.desired_size = desired_size;
        pending_.push_back(req);
    }
    cv_.notify_one();
}

} // namespace exosnap
