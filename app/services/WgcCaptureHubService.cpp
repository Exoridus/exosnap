#include "services/WgcCaptureHubService.h"

#include "diagnostics/AppLog.h"
#include "services/CaptureHubRegistry.h"
#include "services/WgcSourceProducer.h"

#include <recorder_core/preview_shared_texture.h>

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
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

bool WgcCaptureHubService::Subscribe(CaptureSourceKey key, HandleSink sink, FramePublishedSink frame_sink) {
    if (!sink || key.native_id == 0 ||
        (key.kind != CaptureSourceKey::Kind::Monitor && key.kind != CaptureSourceKey::Kind::Window)) {
        return false;
    }
    Command command;
    command.op = Command::Op::Subscribe;
    command.key = std::move(key);
    command.sink = std::move(sink);
    command.frame_sink = std::move(frame_sink);
    PostCommand(std::move(command));
    return true;
}

void WgcCaptureHubService::Unsubscribe() {
    Command command;
    command.op = Command::Op::Unsubscribe;
    PostCommand(std::move(command));
}

void WgcCaptureHubService::RequestEngineLease() {
    Command command;
    command.op = Command::Op::LeaseRequest;
    const uint64_t serial = PostCommand(std::move(command));
    std::unique_lock lock(mutex_);
    if (!ack_cv_.wait_for(lock, std::chrono::milliseconds(750), [&] { return processed_serial_ >= serial; })) {
        diagnostics::AppLog::warning(QStringLiteral("wgc-hub"), QStringLiteral("lease request timed out"));
    }
}

void WgcCaptureHubService::ReturnEngineLease() {
    Command command;
    command.op = Command::Op::LeaseReturn;
    PostCommand(std::move(command));
}

uint64_t WgcCaptureHubService::PostCommand(Command command) {
    uint64_t serial = 0;
    {
        std::lock_guard lock(mutex_);
        serial = next_serial_++;
        command.serial = serial;
        pending_ = std::move(command);
    }
    cv_.notify_all();
    return serial;
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
    recorder_core::PreviewSharedTexture shared;
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
            recorder_core::PreviewTapDesc tap{};
            sink(handle, shared_width, shared_height, tap);
        }
        // A contention drop deliberately does NOT signal: it means the consumer
        // has not taken the previous frame yet, so its redraw is already pending.
        if (shared.TryPublish(producer->Context(), frame.texture.get()) && frame_sink)
            frame_sink();
    };

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
                subscription.Reset();
                producer = nullptr;
                resetPublisher();
                current_key = std::move(command->key);
                sink = std::move(command->sink);
                frame_sink = std::move(command->frame_sink);
                subscription = registry.Subscribe(
                    current_key, [&publish](const HubFrame& frame, recorder_core::HubFrameKind) { publish(frame); });
                break;
            case Command::Op::Unsubscribe:
                subscription.Reset();
                producer = nullptr;
                current_key = {};
                sink = nullptr;
                frame_sink = nullptr;
                resetPublisher();
                break;
            case Command::Op::LeaseRequest:
                if (subscription)
                    registry.RequestLease(current_key);
                resetPublisher();
                break;
            case Command::Op::LeaseReturn:
                if (subscription)
                    registry.ReturnLease(current_key);
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
    subscription.Reset();
    winrt::uninit_apartment();
}

} // namespace exosnap
