#include "services/WgcCaptureHubService.h"

#include "diagnostics/AppLog.h"
#include "services/CaptureHubRegistry.h"
#include "services/WgcSourceProducer.h"

#include <exosnap/engine/device_generation.h>
#include <exosnap/engine/preview_shared_texture.h>
#include <exosnap/engine/preview_tap.h>

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
    // No pump: either the worker never got a device, or it is shutting down.
    // Refusing here is the honest answer -- a queued command against a thread
    // that will never drain it looks like a subscription that is about to work.
    if (commands_.Stopping())
        return false;
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
    // Nothing holds a capture when there is no pump, so there is nothing to wait
    // for. Waiting anyway cost a recording start the full 750 ms per attempt
    // against a service that had already failed to start.
    if (commands_.Stopping())
        return;
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
        diagnostics::AppLog::warning(QStringLiteral("wgc-hub"),
                                     QStringLiteral("could not create D3D11 device; the capture hub is unavailable"));
        // There is no pump from here on, so the service must stop looking like it
        // has one. Without this, Subscribe() kept queueing commands nobody would
        // ever apply and every RequestEngineLease() sat out its full 750 ms
        // waiting for an acknowledgement that could not come -- a recording start
        // paying that cost per attempt, against a service that had already failed.
        // Shutdown() releases existing waiters immediately and makes Stopping()
        // true, which the public entry points check.
        commands_.Shutdown();
        // The apartment was entered above; leaving it to thread exit strands the
        // STA's proxies and RPC channel instead of tearing them down.
        winrt::uninit_apartment();
        return;
    }
    exosnap::engine::DeviceGeneration device_generation = exosnap::engine::NextDeviceGeneration();

    WgcSourceProducer* producer = nullptr;
    // `device` by reference, not by value: a producer built after a rebuild must
    // get the NEW device. Capturing it by value is how every producer created
    // after a DEVICE_REMOVED was handed the dead one.
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
    exosnap::engine::CaptureTapPublishState published;

    const auto resetPublisher = [&]() {
        shared.Reset();
        published = {};
    };
    const auto publish = [&](const HubFrame& frame) {
        if (!frame.texture || producer == nullptr || producer->Device() == nullptr || !sink)
            return;
        D3D11_TEXTURE2D_DESC description{};
        frame.texture->GetDesc(&description);
        // Same rule as the DXGI hub: a shared texture is only reusable while the
        // device it lives on is the device still in use. The dimensions and format
        // are identical across a rebuild, so they cannot answer this.
        const exosnap::engine::CaptureTapFrameState incoming{device_generation,    description.Width,
                                                             description.Height,   description.Format,
                                                             published.hdr_active, published.max_luminance_nits};
        if (exosnap::engine::ShouldRepublishCaptureTap(published, incoming)) {
            shared.Reset();
            HANDLE handle = nullptr;
            std::string error;
            if (!shared.Create(producer->Device(), description.Width, description.Height, description.Format, &handle,
                               error)) {
                published = {};
                diagnostics::AppLog::warning(
                    QStringLiteral("wgc-hub"),
                    QStringLiteral("shared texture create failed: %1").arg(QString::fromStdString(error)));
                return;
            }
            published.device_generation = device_generation;
            published.shared_valid = true;
            published.width = description.Width;
            published.height = description.Height;
            published.format = description.Format;
            exosnap::engine::PreviewTapDesc tap{};
            sink(handle, published.width, published.height, tap);
        }
        // A contention drop deliberately does NOT signal: it means the consumer
        // has not taken the previous frame yet, so its redraw is already pending.
        if (shared.TryPublish(producer->Context(), frame.texture.get()).published() && frame_sink)
            frame_sink();
    };

    // Device-rebuild state, reset whenever the source is live again.
    constexpr exosnap::engine::DeviceRebuildPolicy kRebuildPolicy{};
    uint32_t rebuild_attempts = 0;
    bool rebuild_exhausted = false;
    auto last_rebuild_attempt = std::chrono::steady_clock::now();

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
                rebuild_attempts = 0;
                rebuild_exhausted = false;
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

        // (a) The device died under a live subscription. The hub gave up on the
        // source (its producer reported Fatal) and will not retry, because the
        // producer it holds is built on a device that is gone -- only the owner of
        // that device can replace it. Without this the preview held its last frame
        // for the rest of the session with nothing saying why.
        if (subscription && subscription.SourceLost()) {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t since_last =
                rebuild_attempts == 0
                    ? 0
                    : static_cast<uint64_t>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rebuild_attempt).count());
            const exosnap::engine::DeviceRebuildStep step =
                exosnap::engine::NextDeviceRebuildStep(rebuild_attempts, since_last, kRebuildPolicy);

            if (step == exosnap::engine::DeviceRebuildStep::Rebuild) {
                ++rebuild_attempts;
                last_rebuild_attempt = now;

                // Order matters. The subscription is dropped first so the hub (and
                // its producer built on the dead device) is disposed before a new
                // device exists; the publisher is reset because its shared texture
                // belongs to that device; only then is a device created, and only
                // then may anything be built on it.
                subscription.Reset();
                producer = nullptr;
                resetPublisher();
                device = nullptr;
                device_generation = exosnap::engine::DeviceGeneration{};

                winrt::com_ptr<ID3D11Device> replacement = createDevice();
                if (replacement) {
                    device = std::move(replacement);
                    device_generation = exosnap::engine::NextDeviceGeneration();
                    subscription =
                        registry.Subscribe(current_key, [&publish](const HubFrame& frame,
                                                                   exosnap::engine::HubFrameKind) { publish(frame); });
                    diagnostics::AppLog::info(
                        QStringLiteral("wgc-hub"),
                        QStringLiteral("capture device was replaced; rebuilt the producer (attempt %1)")
                            .arg(rebuild_attempts));
                } else {
                    diagnostics::AppLog::warning(
                        QStringLiteral("wgc-hub"),
                        QStringLiteral("capture device is gone and a replacement could not be created (attempt %1)")
                            .arg(rebuild_attempts));
                }
            } else if (step == exosnap::engine::DeviceRebuildStep::GiveUp && !rebuild_exhausted) {
                // Reported once, and honestly: there is no capture and there will
                // not be one. A consumer that is told nothing keeps waiting for a
                // frame instead of showing that the source is unavailable.
                rebuild_exhausted = true;
                subscription.Reset();
                producer = nullptr;
                resetPublisher();
                diagnostics::AppLog::warning(
                    QStringLiteral("wgc-hub"),
                    QStringLiteral("giving up on the capture device after %1 attempts; the hub is unavailable until a "
                                   "new subscription")
                        .arg(rebuild_attempts));
            }
        } else if (subscription && rebuild_attempts != 0) {
            // A live source again: the next loss gets its own full budget rather
            // than inheriting what this one spent.
            rebuild_attempts = 0;
            rebuild_exhausted = false;
        }
    }
    subscription.Reset();
    winrt::uninit_apartment();
}

} // namespace exosnap
