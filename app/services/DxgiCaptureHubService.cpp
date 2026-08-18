#include "DxgiCaptureHubService.h"

#include "../diagnostics/AppLog.h"
#include "CaptureHubRegistry.h"
#include "DxgiSourceProducer.h"

#include <recorder_core/dxgi_od_capture_src.h>
#include <recorder_core/preview_shared_texture.h>

#include <winrt/base.h>

#include <chrono>
#include <utility>

namespace exosnap {
namespace {

// Pump cadence. A live preview, not a thumbnail grid: the duplication is
// polled non-blocking, so a fast tick costs almost nothing on a quiet desktop.
constexpr std::chrono::milliseconds kPumpTick{15};

// The preview renderer creates its device with D3D11CreateDevice(nullptr,
// D3D_DRIVER_TYPE_HARDWARE, ...), i.e. on the default adapter. A shared
// NT-handle can only be opened on the device that matches the producer's
// adapter, so a monitor driven by any other adapter cannot feed the preview.
bool MonitorIsOnDefaultAdapter(HMONITOR monitor) {
    std::string err;
    winrt::com_ptr<IDXGIAdapter1> monitorAdapter;
    if (!recorder_core::FindAdapterForMonitor(monitor, monitorAdapter.put(), err))
        return false;
    DXGI_ADAPTER_DESC1 monitorDesc{};
    if (FAILED(monitorAdapter->GetDesc1(&monitorDesc)))
        return false;

    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return false;
    winrt::com_ptr<IDXGIAdapter1> defaultAdapter;
    if (FAILED(factory->EnumAdapters1(0, defaultAdapter.put())))
        return false;
    DXGI_ADAPTER_DESC1 defaultDesc{};
    if (FAILED(defaultAdapter->GetDesc1(&defaultDesc)))
        return false;

    return monitorDesc.AdapterLuid.HighPart == defaultDesc.AdapterLuid.HighPart &&
           monitorDesc.AdapterLuid.LowPart == defaultDesc.AdapterLuid.LowPart;
}

} // namespace

DxgiCaptureHubService::DxgiCaptureHubService() {
    worker_ = std::jthread([this](std::stop_token st) { WorkerProc(std::move(st)); });
}

DxgiCaptureHubService::~DxgiCaptureHubService() {
    worker_.request_stop();
    // Wakes the pump thread and releases any lease waiter with a failure rather
    // than leaving it to its full timeout against a thread that is exiting.
    commands_.Shutdown();
    if (worker_.joinable())
        worker_.join();
}

bool DxgiCaptureHubService::Subscribe(HMONITOR monitor, HandleSink sink, FramePublishedSink frame_sink) {
    if (monitor == nullptr || !sink)
        return false;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE || info.szDevice[0] == L'\0') {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-hub"),
                                     QStringLiteral("subscribe: monitor has no stable device name"));
        return false;
    }

    if (!MonitorIsOnDefaultAdapter(monitor)) {
        // D5: explicit subscribe failure, one structured line, WGC fallback at
        // the caller. Never a second duplication for the same output.
        diagnostics::AppLog::warning(
            QStringLiteral("dxgi-hub"),
            QStringLiteral("subscribe: monitor is on a different adapter than the preview; falling back to WGC"));
        return false;
    }

    SubscribePayload payload;
    payload.device_name = info.szDevice;
    payload.sink = std::move(sink);
    payload.frame_sink = std::move(frame_sink);
    commands_.Post(CaptureHubOp::Subscribe, std::move(payload));
    return true;
}

void DxgiCaptureHubService::Unsubscribe() {
    commands_.Post(CaptureHubOp::Unsubscribe);
}

void DxgiCaptureHubService::RequestEngineLease() {
    const uint64_t serial = commands_.Post(CaptureHubOp::LeaseRequest);
    // Only this command's own release publishes an acknowledgement for this
    // serial, and the service opens nothing further until the lease returns —
    // so the wait ends on "the duplication is gone", never on "something else
    // happened afterwards".
    if (!commands_.WaitForLeaseRelease(serial, std::chrono::milliseconds(750))) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-hub"),
                                     QStringLiteral("lease request timed out; the engine's capture open may fail"));
    }
}

void DxgiCaptureHubService::ReturnEngineLease() {
    commands_.Post(CaptureHubOp::LeaseReturn);
}

DxgiCaptureHubService::PreviewPublishStats DxgiCaptureHubService::GetPreviewPublishStats() const noexcept {
    return {
        publish_attempts_.load(std::memory_order_relaxed),
        published_frames_.load(std::memory_order_relaxed),
        publish_drops_.load(std::memory_order_relaxed),
    };
}

void DxgiCaptureHubService::ResetPreviewPublishStats() noexcept {
    publish_attempts_.store(0, std::memory_order_relaxed);
    published_frames_.store(0, std::memory_order_relaxed);
    publish_drops_.store(0, std::memory_order_relaxed);
}

void DxgiCaptureHubService::WorkerProc(std::stop_token stop_token) {
    // Everything below is pump-thread-owned. The producer pointer is only valid
    // while the subscription that created it lives: the registry disposes the
    // hub (and its producer) when the last consumer leaves.
    DxgiSourceProducer* producer = nullptr;
    CaptureHubRegistry registry([&producer](const CaptureSourceKey& key) {
        auto p = std::make_unique<DxgiSourceProducer>(key.device_name);
        producer = p.get();
        return p;
    });

    CaptureSubscription subscription;
    HandleSink sink;
    FramePublishedSink frameSink;
    CaptureSourceKey currentKey;

    // The service-level lease/subscription arbitration (CaptureHubGate.h) and
    // the payload a deferred Subscribe is waiting to be applied with.
    CaptureHubGateState gate;
    SubscribePayload desired;

    // Publisher state: the shared texture lives on the producer's device and is
    // recreated whenever the desktop's size or format changes.
    recorder_core::PreviewSharedTexture shared;
    uint32_t sharedW = 0;
    uint32_t sharedH = 0;
    DXGI_FORMAT sharedFmt = DXGI_FORMAT_UNKNOWN;
    bool announceFailed = false;
    // Last HDR facts the tap descriptor was resolved from. A live Windows-HDR
    // toggle (or Auto-HDR) can leave desc.{Width,Height,Format} unchanged — an
    // Advanced-Color desktop keeps delivering FP16 in both states — so those
    // alone are not sufficient to notice the tap has gone stale; hdr_active /
    // max_luminance_nits must be compared every tick too, or the preview keeps
    // tone-mapping with the peak/mode from whenever the texture was last
    // (re)created, silently disagreeing with the display's current HDR state.
    bool lastHdrActive = false;
    float lastMaxLuminanceNits = 0.0f;

    const auto resetPublisher = [&]() {
        shared.Reset();
        sharedW = 0;
        sharedH = 0;
        sharedFmt = DXGI_FORMAT_UNKNOWN;
        announceFailed = false;
        lastHdrActive = false;
        lastMaxLuminanceNits = 0.0f;
    };

    const auto publish = [&](const HubFrame& frame) {
        if (!frame.texture || announceFailed || producer == nullptr || producer->Device() == nullptr)
            return;
        D3D11_TEXTURE2D_DESC desc{};
        frame.texture->GetDesc(&desc);
        const recorder_core::HdrDisplayFacts& facts = producer->DisplayFacts();
        if (recorder_core::ShouldRepublishCaptureTap(shared.Valid(), sharedW, sharedH, sharedFmt, desc.Width,
                                                     desc.Height, desc.Format, lastHdrActive, lastMaxLuminanceNits,
                                                     facts.hdr_active, facts.max_luminance_nits)) {
            HANDLE handle = nullptr;
            std::string err;
            if (!shared.Create(producer->Device(), desc.Width, desc.Height, desc.Format, &handle, err)) {
                announceFailed = true;
                diagnostics::AppLog::warning(
                    QStringLiteral("dxgi-hub"),
                    QStringLiteral("shared texture create failed: %1").arg(QString::fromStdString(err)));
                return;
            }
            sharedW = desc.Width;
            sharedH = desc.Height;
            sharedFmt = desc.Format;
            lastHdrActive = facts.hdr_active;
            lastMaxLuminanceNits = facts.max_luminance_nits;
            const recorder_core::PreviewTapDesc tap =
                recorder_core::ResolveRawCaptureTapDesc(desc.Format, facts.hdr_active, facts.max_luminance_nits);
            // Ownership of the NT handle transfers to the sink.
            sink(handle, sharedW, sharedH, tap);
        }
        publish_attempts_.fetch_add(1, std::memory_order_relaxed);
        if (shared.TryPublish(producer->Context(), frame.texture.get())) {
            published_frames_.fetch_add(1, std::memory_order_relaxed);
            // A contention drop deliberately does NOT signal: it means the
            // consumer still has the previous frame to take, so a redraw is
            // already on its way to it.
            if (frameSink)
                frameSink();
        } else {
            publish_drops_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<CaptureHubCommandQueue<SubscribePayload>::Entry> batch;

    while (!stop_token.stop_requested()) {
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

            // Drop before open, always: refcount to zero closes the duplication
            // BEFORE any new one opens (one duplication per output, ever).
            if (action.drop_subscription) {
                subscription.Reset();
                producer = nullptr;
                sink = nullptr;
                frameSink = nullptr;
                currentKey = {};
            }
            if (action.release_lease)
                registry.RequestLease(currentKey);
            if (action.reset_publisher)
                resetPublisher();
            if (action.return_lease)
                registry.ReturnLease(currentKey);
            if (action.apply_subscription) {
                sink = std::move(desired.sink);
                frameSink = std::move(desired.frame_sink);
                currentKey = {};
                currentKey.kind = CaptureSourceKey::Kind::DxgiMonitor;
                currentKey.device_name = desired.device_name;
                subscription = registry.Subscribe(
                    currentKey, [&publish](const HubFrame& frame, recorder_core::HubFrameKind) { publish(frame); });
                diagnostics::AppLog::debug(QStringLiteral("dxgi-hub"), QStringLiteral("subscribed to display feed"));
            }
            if (action.acknowledge_release) {
                // Published only now: after this point the gate refuses to open
                // anything until the lease is returned, so the engine's wait
                // ends on a duplication that is gone and stays gone.
                commands_.PublishLeaseRelease(command.serial);
                diagnostics::AppLog::debug(QStringLiteral("dxgi-hub"), QStringLiteral("lease granted to the engine"));
            }
            if (command.op == CaptureHubOp::LeaseReturn)
                diagnostics::AppLog::debug(QStringLiteral("dxgi-hub"), QStringLiteral("lease returned; reopening"));
        }

        registry.PumpAll();
    }

    // Subscriptions must die before the registry that owns their hubs.
    subscription.Reset();
}

} // namespace exosnap
