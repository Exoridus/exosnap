#include "services/WgcSourceProducer.h"

#include <utility>

#include <windows.h>

#include <dxgi.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>

#include <exosnap/engine/wgc_acquire_classify.h>

#include "../diagnostics/AppLog.h"

namespace exosnap {

namespace {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

constexpr auto kPoolFormat = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int kPoolBuffers = 2;

} // namespace

WgcSourceProducer::WgcSourceProducer(CaptureSourceKey key, winrt::com_ptr<ID3D11Device> device)
    : key_(key), device_(std::move(device)) {
}

WgcSourceProducer::~WgcSourceProducer() {
    Close();
}

bool WgcSourceProducer::Open(std::string& err) {
    if (!device_) {
        err = "no d3d device";
        return false;
    }

    try {
        device_->GetImmediateContext(context_.put());

        winrt::com_ptr<IDXGIDevice> dxgi_dev = device_.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.get(), insp.put()));
        winrt_device_ = insp.as<wgdx::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (key_.kind == CaptureSourceKey::Kind::Monitor) {
            winrt::check_hresult(interop->CreateForMonitor(reinterpret_cast<HMONITOR>(key_.native_id),
                                                           winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                           winrt::put_abi(item_)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(reinterpret_cast<HWND>(key_.native_id),
                                                          winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                          winrt::put_abi(item_)));
        }

        pool_size_ = item_.Size();
        if (pool_size_.Width <= 0 || pool_size_.Height <= 0) {
            err = "capture item has empty size";
            Close();
            return false;
        }

        frame_pool_ = wgc::Direct3D11CaptureFramePool::Create(winrt_device_, kPoolFormat, kPoolBuffers, pool_size_);
        session_ = frame_pool_.CreateCaptureSession(item_);
        session_.IsBorderRequired(false);

        // Cleared before the handler exists, never after: a Closed that fires on
        // the very first pump must not be erased by this producer's own reset.
        source_closed_.store(false);
        closed_token_ = item_.Closed([this](const auto&, const auto&) { source_closed_.store(true); });

        session_.StartCapture();
    } catch (const winrt::hresult_error& e) {
        err = winrt::to_string(e.message());
        Close();
        return false;
    }

    return true;
}

void WgcSourceProducer::Close() {
    if (item_ != nullptr && closed_token_)
        item_.Closed(closed_token_);
    closed_token_ = {};

    if (session_ != nullptr)
        session_.Close();
    session_ = nullptr;

    if (frame_pool_ != nullptr)
        frame_pool_.Close();
    frame_pool_ = nullptr;

    item_ = nullptr;
    winrt_device_ = nullptr;
    context_ = nullptr;
    pool_size_ = {};
    source_closed_.store(false);
}

ProducerPoll WgcSourceProducer::PollFrame(HubFrame& out) {
    if (source_closed_.load())
        return ProducerPoll::Lost;

    // Polling a producer that never opened is a caller's bug, but a null projected
    // type dereferences rather than throwing, so answer instead of crashing.
    if (frame_pool_ == nullptr)
        return ProducerPoll::Lost;

    try {
        auto frame = frame_pool_.TryGetNextFrame();
        if (frame == nullptr)
            return ProducerPoll::NoFrame; // A quiet desktop is not a lost one.

        // TryGetNextFrame hands back the OLDEST queued frame. A source producing
        // at refresh rate outruns any hub that polls slower, so taking the first
        // frame would walk a backlog: the consumer would see old images and the
        // queue would never drain. Drain to the newest and drop the rest.
        for (;;) {
            auto newer = frame_pool_.TryGetNextFrame();
            if (newer == nullptr)
                break;
            frame = newer;
        }

        const auto content = frame.ContentSize();
        if (content.Width != pool_size_.Width || content.Height != pool_size_.Height) {
            // The source renegotiated its resolution. Resize the pool to match
            // and let the next tick deliver the first frame at the new size.
            pool_size_ = content;
            frame_pool_.Recreate(winrt_device_, kPoolFormat, kPoolBuffers, pool_size_);
            return ProducerPoll::NoFrame;
        }

        auto surface = frame.Surface();
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> frame_tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(frame_tex.put())));

        D3D11_TEXTURE2D_DESC desc{};
        frame_tex->GetDesc(&desc);

        if (!copy_texture_ || copy_width_ != desc.Width || copy_height_ != desc.Height) {
            copy_texture_ = nullptr;
            D3D11_TEXTURE2D_DESC copy_desc{};
            copy_desc.Width = desc.Width;
            copy_desc.Height = desc.Height;
            copy_desc.MipLevels = 1;
            copy_desc.ArraySize = 1;
            copy_desc.Format = desc.Format;
            copy_desc.SampleDesc.Count = 1;
            copy_desc.Usage = D3D11_USAGE_DEFAULT;
            copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            winrt::check_hresult(device_->CreateTexture2D(&copy_desc, nullptr, copy_texture_.put()));
            copy_width_ = desc.Width;
            copy_height_ = desc.Height;
        }

        // The frame pool recycles its surfaces, so the held frame must be a copy.
        // No blank-frame detection: it needs a per-pixel readback, it is
        // unreliable, and the hold does not need it -- a consumer reading
        // HeldFrame() already survives a source that merely stops producing.
        context_->CopyResource(copy_texture_.get(), frame_tex.get());

        out.texture = copy_texture_;
        out.width = copy_width_;
        out.height = copy_height_;
        out.generation = ++generation_;
        return ProducerPoll::Frame;
    } catch (const winrt::hresult_error& e) {
        // Same classification the recording worker applies, so one HRESULT never
        // means two different things in one product. The ACTION differs by design:
        // a preview holds its last frame where a recording ends the session.
        switch (exosnap::engine::ClassifyWgcAcquireFailure(e.code().value)) {
        case exosnap::engine::WgcAcquireFailure::DeviceLost:
            return ProducerPoll::Fatal;
        case exosnap::engine::WgcAcquireFailure::SourceLost:
            return ProducerPoll::Lost;
        case exosnap::engine::WgcAcquireFailure::Unexpected:
            // Held like a source loss -- a preview must not take the app down --
            // but never silently: an unclassified code here is the one thing that
            // would otherwise look identical to an ordinary blank.
            diagnostics::AppLog::warning(QStringLiteral("wgc-producer"),
                                         QStringLiteral("unexpected acquire failure 0x%1")
                                             .arg(static_cast<uint32_t>(e.code().value), 8, 16, QLatin1Char('0')));
            return ProducerPoll::Lost;
        }
        return ProducerPoll::Lost;
    }
}

} // namespace exosnap
