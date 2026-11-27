#include "services/CaptureTargetStillService.h"

#include "services/CaptureSourceKey.h"
#include "services/WgcSourceProducer.h"

#include "../diagnostics/AppLog.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include <windows.h>

#include <d3d11.h>

#include <winrt/base.h>

namespace exosnap {
namespace {

using namespace std::chrono_literals;

// How long one target may take to hand over its first frame. A window that is
// minimized, cloaked or behind the secure desktop never produces one, and the
// round robin must not stall on it.
constexpr auto kFrameWait = 150ms;

// Only used when the producer could not hand out a frame-arrival event. With one
// the wait blocks until WGC delivers, and this cadence never applies.
constexpr auto kPollSlice = 5ms;
constexpr int kFailuresBeforeUnavailable = 2;
constexpr std::size_t kMaxCachedItems = 32;

void pumpCaptureMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

winrt::com_ptr<ID3D11Device> createDevice() {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    const HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                             static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(),
                                             &selected, context.put());
    return SUCCEEDED(result) ? device : nullptr;
}

CaptureSourceKey keyFor(const exosnap::engine::CaptureTarget& target) {
    CaptureSourceKey key;
    key.kind = target.kind == exosnap::engine::CaptureTarget::Kind::Window ? CaptureSourceKey::Kind::Window
                                                                           : CaptureSourceKey::Kind::Monitor;
    key.native_id = target.native_id;
    return key;
}

UINT fullMipChain(UINT width, UINT height) {
    UINT levels = 1;
    UINT edge = std::max(width, height);
    while (edge > 1) {
        edge /= 2;
        ++levels;
    }
    return levels;
}

// The mip that still covers kMaxStillEdge. Reading back mip 0 of a 4K window
// would move 33 MB across PCIe four times a second for an 84 px box; the GPU
// filters the chain for free and the readback drops to a few hundred KB.
UINT stillMipLevel(UINT width, UINT height, UINT level_count) {
    UINT level = 0;
    for (UINT candidate = 1; candidate < level_count; ++candidate) {
        const UINT edge = std::max(width >> candidate, height >> candidate);
        if (edge < static_cast<UINT>(CaptureTargetStillService::kMaxStillEdge))
            break;
        level = candidate;
    }
    return level;
}

QImage stillFromTexture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    if (device == nullptr || context == nullptr || texture == nullptr)
        return {};

    D3D11_TEXTURE2D_DESC source_desc{};
    texture->GetDesc(&source_desc);
    if (source_desc.Width == 0 || source_desc.Height == 0)
        return {};
    // The frame pool is pinned to BGRA8, and the readback below assumes it.
    // A different format is a contract change upstream, not something to
    // reinterpret here.
    if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return {};

    const UINT level_count = fullMipChain(source_desc.Width, source_desc.Height);
    D3D11_TEXTURE2D_DESC mipped_desc{};
    mipped_desc.Width = source_desc.Width;
    mipped_desc.Height = source_desc.Height;
    mipped_desc.MipLevels = level_count;
    mipped_desc.ArraySize = 1;
    mipped_desc.Format = source_desc.Format;
    mipped_desc.SampleDesc.Count = 1;
    mipped_desc.Usage = D3D11_USAGE_DEFAULT;
    mipped_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    mipped_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    winrt::com_ptr<ID3D11Texture2D> mipped;
    if (FAILED(device->CreateTexture2D(&mipped_desc, nullptr, mipped.put())))
        return {};

    winrt::com_ptr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(mipped.get(), nullptr, view.put())))
        return {};

    context->CopySubresourceRegion(mipped.get(), 0, 0, 0, 0, texture, 0, nullptr);
    context->GenerateMips(view.get());

    const UINT level = stillMipLevel(source_desc.Width, source_desc.Height, level_count);
    const UINT level_width = std::max<UINT>(source_desc.Width >> level, 1);
    const UINT level_height = std::max<UINT>(source_desc.Height >> level, 1);

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = level_width;
    staging_desc.Height = level_height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = source_desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, staging.put())))
        return {};

    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, mipped.get(), level, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return {};

    // Format_RGB32 is BGRX: the byte order already matches, and dropping alpha
    // is deliberate. A window with per-pixel transparency would otherwise show
    // the card behind it through its own thumbnail.
    QImage image(static_cast<int>(level_width), static_cast<int>(level_height), QImage::Format_RGB32);
    const auto* rows = static_cast<const uchar*>(mapped.pData);
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(image.scanLine(y), rows + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(image.width()) * 4);
    context->Unmap(staging.get(), 0);

    const int edge = CaptureTargetStillService::kMaxStillEdge;
    if (image.width() > edge || image.height() > edge)
        image = image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace

CaptureTargetStillService::CaptureTargetStillService(QObject* parent)
    : QObject(parent), activity_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
}

CaptureTargetStillService::~CaptureTargetStillService() {
    stop();
}

void CaptureTargetStillService::start() {
    if (worker_.joinable())
        return;
    worker_ = std::jthread([this](std::stop_token stop_token) { workerProc(std::move(stop_token)); });
}

void CaptureTargetStillService::stop() {
    if (!worker_.joinable())
        return;
    worker_.request_stop();
    wakeWorker();
    worker_.join();
    std::lock_guard lock(mutex_);
    visible_.clear();
    cursor_ = 0;
    consecutive_failures_.clear();
    ++targets_revision_;
}

void CaptureTargetStillService::setVisibleTargets(std::vector<Request> targets) {
    {
        std::lock_guard lock(mutex_);
        visible_ = std::move(targets);
        ++targets_revision_;
        std::erase_if(consecutive_failures_, [this](const auto& entry) {
            return std::ranges::none_of(visible_,
                                        [&entry](const auto& request) { return request.identity == entry.first; });
        });
        if (cursor_ >= visible_.size())
            cursor_ = 0;
    }
    wakeWorker();
}

void CaptureTargetStillService::wakeWorker() {
    if (activity_event_)
        SetEvent(activity_event_.get());
    wake_.notify_all();
}

bool CaptureTargetStillService::isCurrent(std::uint64_t revision) const {
    std::lock_guard lock(mutex_);
    return revision == targets_revision_;
}

void CaptureTargetStillService::publish(Request request, std::uint64_t revision, QImage still, bool invalid_identity) {
    QMetaObject::invokeMethod(
        this,
        [this, request = std::move(request), revision, still = std::move(still), invalid_identity] {
            // A superseded queued result must not consume the unavailable transition.
            if (!isCurrent(revision))
                return;
            if (!still.isNull()) {
                consecutive_failures_.erase(request.identity);
            } else if (!invalid_identity) {
                int& failures = consecutive_failures_[request.identity];
                if (failures == kFailuresBeforeUnavailable || ++failures < kFailuresBeforeUnavailable)
                    return;
            }
            if (still.isNull() || (request.valid && !request.valid()))
                emit stillUnavailable(request.identity);
            else
                emit stillReady(request.identity, still);
        },
        Qt::QueuedConnection);
}

void CaptureTargetStillService::workerProc(std::stop_token stop_token) {
    // WGC delivers the item.Closed callback through the apartment's message
    // queue, so the grab loop below pumps it. Same contract WgcSourceProducer
    // documents and the capture hub already satisfies.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::com_ptr<ID3D11Device> device;
    struct CachedItem {
        QString identity;
        std::shared_ptr<WgcCaptureItem> item;
    };
    // Retain recently used items across viewport changes because WGC can retain
    // a port after each creation and release.
    std::vector<CachedItem> cached_items;
    cached_items.reserve(kMaxCachedItems);
    const HANDLE activity = activity_event_.get();
    while (!stop_token.stop_requested()) {
        Request request;
        std::uint64_t revision = 0;
        bool idle = false;
        {
            std::unique_lock lock(mutex_);
            if (!visible_.empty())
                wake_.wait_for(lock, stop_token, std::chrono::milliseconds(kGrabIntervalMs),
                               [&] { return visible_.empty(); });
            if (stop_token.stop_requested())
                break;
            revision = targets_revision_;
            idle = visible_.empty();
            if (!idle) {
                request = visible_[cursor_];
                cursor_ = (cursor_ + 1) % visible_.size();
            }
        }

        pumpCaptureMessages();
        std::erase_if(cached_items, [](const auto& entry) { return entry.item->IsClosed(); });
        if (idle) {
            // WGC can close a retained item while the picker has no capture session or D3D device.
            device = nullptr;
            if (activity != nullptr)
                MsgWaitForMultipleObjectsEx(1, &activity, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            else {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop_token, [&] { return !visible_.empty(); });
            }
            continue;
        }
        if (!device) {
            device = createDevice();
            if (!device) {
                diagnostics::AppLog::warning(
                    QStringLiteral("target-stills"),
                    QStringLiteral("could not create D3D11 device; picker keeps its placeholders"));
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop_token, [&] { return revision != targets_revision_; });
                continue;
            }
        }

        const auto key = keyFor(request.target);
        std::shared_ptr<WgcCaptureItem> item;
        const bool is_window = key.kind == CaptureSourceKey::Kind::Window;
        const HWND window = is_window ? reinterpret_cast<HWND>(key.native_id) : nullptr;
        const bool may_reuse = is_window && static_cast<bool>(request.valid);
        const auto found = std::ranges::find(cached_items, request.identity, &CachedItem::identity);
        if (found != cached_items.end()) {
            if (may_reuse)
                item = std::move(found->item);
            cached_items.erase(found);
        }
        if (request.valid && !request.valid()) {
            publish(std::move(request), revision, {}, true);
            continue;
        }

        WgcSourceProducer producer(key, device);
        // Asked for before Open, which is when the producer can still subscribe.
        const HANDLE frame_signal = producer.FrameSignal();
        // A stop request sets the same event, so teardown does not have to sit
        // out the frame wait a window that never draws would otherwise cost.
        std::stop_callback wake_on_stop(stop_token, [frame_signal] {
            if (frame_signal != nullptr)
                SetEvent(frame_signal);
        });
        std::string error;
        QImage still;
        bool device_lost = false;
        const bool capturable = !is_window || (IsWindowVisible(window) && !IsIconic(window));
        const bool opened = capturable && isCurrent(revision) && producer.Open(error, item);
        bool source_lost = capturable && !opened && !error.empty();
        if (opened) {
            const auto deadline = std::chrono::steady_clock::now() + kFrameWait;
            while (!stop_token.stop_requested() && isCurrent(revision)) {
                pumpCaptureMessages();
                HubFrame frame;
                const ProducerPoll poll = producer.PollFrame(frame);
                if (request.valid && !request.valid())
                    break;
                if (poll == ProducerPoll::Frame) {
                    still = stillFromTexture(device.get(), producer.Context(), frame.texture.get());
                    if (request.valid && !request.valid())
                        still = {};
                    break;
                }
                if (poll == ProducerPoll::Lost) {
                    source_lost = true;
                    break;
                }
                if (poll == ProducerPoll::Fatal) {
                    device_lost = true;
                    break;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline || stop_token.stop_requested())
                    break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

                // Wait for WGC to say there is a frame instead of asking again in
                // a few milliseconds. FrameArrived is marshalled through this
                // thread's pump like every other WGC callback, so the wait has to
                // wake on messages as well -- the event only tells a frame apart
                // from any other wake.
                if (frame_signal != nullptr) {
                    const HANDLE wait_handles[] = {frame_signal, activity};
                    MsgWaitForMultipleObjectsEx(activity != nullptr ? 2 : 1, wait_handles,
                                                static_cast<DWORD>(remaining.count()), QS_ALLINPUT,
                                                MWMO_INPUTAVAILABLE);
                } else {
                    std::this_thread::sleep_for(std::min(kPollSlice, remaining));
                }
            }
        }
        producer.Close();

        if (stop_token.stop_requested())
            break;
        if (device_lost || FAILED(device->GetDeviceRemovedReason())) {
            device = nullptr;
            still = {};
        }

        if (request.valid && !request.valid()) {
            publish(std::move(request), revision, {}, true);
            continue;
        }
        if (source_lost || (item && item->IsClosed())) {
            still = {};
        } else if (may_reuse && item) {
            if (cached_items.size() == kMaxCachedItems)
                cached_items.erase(cached_items.begin());
            cached_items.push_back({request.identity, std::move(item)});
        }
        publish(std::move(request), revision, std::move(still));
    }

    cached_items.clear();
    device = nullptr;
    winrt::uninit_apartment();
}

} // namespace exosnap
