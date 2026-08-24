#include "services/WgcCaptureHubService.h"

#include "diagnostics/AppLog.h"
#include "services/CaptureHubRegistry.h"
#include "services/WgcSourceProducer.h"

#include <exosnap/engine/preview_shared_texture.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <chrono>
#include <iterator>
#include <utility>

namespace exosnap {
namespace {

constexpr std::chrono::milliseconds kPumpTick{15};

winrt::com_ptr<ID3D11Device> createDevice() {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), &selected,
                                   context.put());
#if defined(_DEBUG)
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), &selected,
                               context.put());
    }
#endif
    return SUCCEEDED(hr) ? device : nullptr;
}

} // namespace

WgcCaptureHubService::WgcCaptureHubService() {
    worker_ = std::jthread([this](std::stop_token stop_token) { WorkerProc(std::move(stop_token)); });
}

WgcCaptureHubService::~WgcCaptureHubService() {
    worker_.request_stop();
    // Wakes the pump thread and releases any lease waiter with a failure rather
    // than leaving it to its full timeout against a thread that is exiting.
    commands_.Shutdown();
    if (worker_.joinable())
        worker_.join();
}

bool WgcCaptureHubService::Subscribe(CaptureSourceKey key, HandleSink sink, FramePublishedSink frame_sink) {
    if (!sink || key.native_id == 0 ||
        (key.kind != CaptureSourceKey::Kind::Monitor && key.kind != CaptureSourceKey::Kind::Window)) {
        return false;
    }
    SubscribePayload payload;
    payload.key = std::move(key);
    payload.sink = std::move(sink);
    payload.frame_sink = std::move(frame_sink);
    commands_.Post(CaptureHubOp::Subscribe, std::move(payload));
    return true;
}

void WgcCaptureHubService::Unsubscribe() {
    commands_.Post(CaptureHubOp::Unsubscribe);
}

void WgcCaptureHubService::RequestEngineLease() {
    const uint64_t serial = commands_.Post(CaptureHubOp::LeaseRequest);
    // Only this command's own release publishes an acknowledgement for this
    // serial, and the service opens nothing further until the lease returns.
    if (!commands_.WaitForLeaseRelease(serial, std::chrono::milliseconds(750)))
        diagnostics::AppLog::warning(QStringLiteral("wgc-hub"), QStringLiteral("lease request timed out"));
}

void WgcCaptureHubService::ReturnEngineLease() {
    commands_.Post(CaptureHubOp::LeaseReturn);
}

void WgcCaptureHubService::WorkerProc(std::stop_token stop_token) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::com_ptr<ID3D11Device> device = createDevice();
    if (!device) {
        diagnostics::AppLog::warning(QStringLiteral("wgc-hub"), QStringLiteral("could not create D3D11 device"));
        // The apartment was entered above; leaving it to thread exit strands the
        // STA's proxies and RPC channel instead of tearing them down.
        winrt::uninit_apartment();
        return;
    }

    WgcSourceProducer* producer = nullptr;
    CaptureHubRegistry registry([&](const CaptureSourceKey& key) {
        auto value = std::make_unique<WgcSourceProducer>(key, device);
        producer = value.get();
        return value;
    });
    CaptureSubscription subscription;
    CaptureSourceKey current_key;
    HandleSink sink;
    FramePublishedSink frame_sink;

    // The service-level lease/subscription arbitration (CaptureHubGate.h) and
    // the payload a deferred Subscribe is waiting to be applied with.
    CaptureHubGateState gate;
    SubscribePayload desired;

    exosnap::engine::PreviewSharedTexture shared;
    uint32_t shared_width = 0;
    uint32_t shared_height = 0;
    DXGI_FORMAT shared_format = DXGI_FORMAT_UNKNOWN;

    const auto resetPublisher = [&]() {
        shared.Reset();
        shared_width = 0;
        shared_height = 0;
        shared_format = DXGI_FORMAT_UNKNOWN;
    };
    const auto publish = [&](const HubFrame& frame) {
        if (!frame.texture || producer == nullptr || producer->Device() == nullptr || !sink)
            return;
        D3D11_TEXTURE2D_DESC description{};
        frame.texture->GetDesc(&description);
        if (!shared.Valid() || shared_width != description.Width || shared_height != description.Height ||
            shared_format != description.Format) {
            HANDLE handle = nullptr;
            std::string error;
            if (!shared.Create(producer->Device(), description.Width, description.Height, description.Format, &handle,
                               error)) {
                diagnostics::AppLog::warning(
                    QStringLiteral("wgc-hub"),
                    QStringLiteral("shared texture create failed: %1").arg(QString::fromStdString(error)));
                return;
            }
            shared_width = description.Width;
            shared_height = description.Height;
            shared_format = description.Format;
            exosnap::engine::PreviewTapDesc tap{};
            sink(handle, shared_width, shared_height, tap);
        }
        // A contention drop deliberately does NOT signal: it means the consumer
        // has not taken the previous frame yet, so its redraw is already pending.
        if (shared.TryPublish(producer->Context(), frame.texture.get()).published() && frame_sink)
            frame_sink();
    };

    std::vector<CaptureHubCommandQueue<SubscribePayload>::Entry> batch;

    while (!stop_token.stop_requested()) {
        // WgcSourceProducer's contract (see its header) is that the owning thread
        // is an STA *that pumps messages*: Windows.Graphics.Capture delivers both
        // frames and the GraphicsCaptureItem.Closed callback through that pump.
        // Every other WGC caller in the tree drains the queue; this one did not,
        // so the Closed handler never fired, source_closed_ stayed false, and a
        // capture window that had been destroyed was never reported as Lost --
        // the preview simply held its last frame forever.
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        commands_.WaitAndDrain(kPumpTick, batch);
        if (stop_token.stop_requested())
            break;

        // Every drained command is applied, in post order: nothing is dropped
        // because something newer arrived while the pump was busy.
        for (auto& command : batch) {
            const CaptureHubGateAction action = StepCaptureHubGate(gate, command.op);
            gate = action.next;

            if (command.op == CaptureHubOp::Subscribe)
                desired = std::move(command.payload);

            if (action.drop_subscription) {
                subscription.Reset();
                producer = nullptr;
                current_key = {};
                sink = nullptr;
                frame_sink = nullptr;
            }
            if (action.release_lease)
                registry.RequestLease(current_key);
            if (action.reset_publisher)
                resetPublisher();
            if (action.return_lease)
                registry.ReturnLease(current_key);
            if (action.apply_subscription) {
                current_key = desired.key;
                sink = std::move(desired.sink);
                frame_sink = std::move(desired.frame_sink);
                subscription = registry.Subscribe(
                    current_key, [&publish](const HubFrame& frame, exosnap::engine::HubFrameKind) { publish(frame); });
            }
            if (action.acknowledge_release) {
                // Published only now: after this point the gate refuses to open
                // anything until the lease is returned, so the engine's wait
                // ends on a capture that is gone and stays gone.
                commands_.PublishLeaseRelease(command.serial);
            }
        }
        registry.PumpAll();
    }
    subscription.Reset();
    winrt::uninit_apartment();
}

} // namespace exosnap
