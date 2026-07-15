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
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

bool DxgiCaptureHubService::Subscribe(HMONITOR monitor, HandleSink sink) {
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

    Command cmd;
    cmd.op = Command::Op::Subscribe;
    cmd.device_name = info.szDevice;
    cmd.sink = std::move(sink);
    PostCommand(std::move(cmd));
    return true;
}

void DxgiCaptureHubService::Unsubscribe() {
    Command cmd;
    cmd.op = Command::Op::Unsubscribe;
    PostCommand(std::move(cmd));
}

void DxgiCaptureHubService::RequestEngineLease() {
    Command cmd;
    cmd.op = Command::Op::LeaseRequest;
    const uint64_t serial = PostCommand(std::move(cmd));

    std::unique_lock lock(mutex_);
    // A later command's serial also satisfies the wait: the pump processes the
    // newest pending command, and every command handler leaves no duplication
    // open unless it (re)subscribed — which only this thread could have asked
    // for after the lease request. "Processed >= our serial" therefore implies
    // the duplication is closed.
    const bool released =
        ack_cv_.wait_for(lock, std::chrono::milliseconds(750), [&] { return processed_serial_ >= serial; });
    if (!released) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-hub"),
                                     QStringLiteral("lease request timed out; the engine's capture open may fail"));
    }
}

void DxgiCaptureHubService::ReturnEngineLease() {
    Command cmd;
    cmd.op = Command::Op::LeaseReturn;
    PostCommand(std::move(cmd));
}

uint64_t DxgiCaptureHubService::PostCommand(Command cmd) {
    uint64_t serial = 0;
    {
        std::lock_guard lock(mutex_);
        serial = next_serial_++;
        cmd.serial = serial;
        pending_ = std::move(cmd);
    }
    cv_.notify_all();
    return serial;
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
    CaptureSourceKey currentKey;

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
        shared.TryPublish(producer->Context(), frame.texture.get());
    };

    while (!stop_token.stop_requested()) {
        std::optional<Command> command;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, kPumpTick, [&] { return pending_.has_value() || stop_token.stop_requested(); });
            command = std::exchange(pending_, std::nullopt);
        }
        if (stop_token.stop_requested())
            break;

        if (command) {
            switch (command->op) {
            case Command::Op::Subscribe:
                // Old subscription first: refcount to zero closes the duplication
                // BEFORE any new one opens (one duplication per output, ever).
                subscription.Reset();
                producer = nullptr;
                sink = nullptr;
                resetPublisher();

                sink = std::move(command->sink);
                currentKey = {};
                currentKey.kind = CaptureSourceKey::Kind::DxgiMonitor;
                currentKey.device_name = std::move(command->device_name);
                subscription = registry.Subscribe(
                    currentKey, [&publish](const HubFrame& frame, recorder_core::HubFrameKind) { publish(frame); });
                diagnostics::AppLog::debug(QStringLiteral("dxgi-hub"), QStringLiteral("subscribed to display feed"));
                break;

            case Command::Op::Unsubscribe:
                subscription.Reset();
                producer = nullptr;
                sink = nullptr;
                currentKey = {};
                resetPublisher();
                break;

            case Command::Op::LeaseRequest:
                // The engine takes the capture; the subscription and the hub's
                // held frame stay. The producer's device dies with its close,
                // so the publisher must rebuild on the next frame after return.
                if (subscription)
                    registry.RequestLease(currentKey);
                resetPublisher();
                diagnostics::AppLog::debug(QStringLiteral("dxgi-hub"), QStringLiteral("lease granted to the engine"));
                break;

            case Command::Op::LeaseReturn:
                if (subscription)
                    registry.ReturnLease(currentKey);
                diagnostics::AppLog::debug(QStringLiteral("dxgi-hub"), QStringLiteral("lease returned; reopening"));
                break;
            }

            {
                std::lock_guard lock(mutex_);
                processed_serial_ = command->serial;
            }
            ack_cv_.notify_all();
        }

        registry.PumpAll();
    }

    // Subscriptions must die before the registry that owns their hubs.
    subscription.Reset();
}

} // namespace exosnap
