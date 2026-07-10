#include "ThumbnailCapture.h"

#include "services/CaptureHubRegistry.h"
#include "services/CaptureSourceKey.h"
#include "services/ThumbnailMip.h"
#include "services/WgcSourceProducer.h"

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <utility>

#include <windows.h>

#include <d3d11.h>

namespace exosnap {
namespace {

using Clock = std::chrono::steady_clock;

// The loop wakes this often to service the apartment's message queue, which is
// how WGC delivers item.Closed.
constexpr auto kTick = std::chrono::milliseconds(33);

// Each hub is stepped -- and therefore each source copied -- at this rate. It
// need not match the tick: a tile is not a preview, and every step costs a
// full-resolution GPU copy per subscribed source.
constexpr auto kPumpInterval = std::chrono::milliseconds(100);

// How often a tile is read back, downscaled and pushed to the UI. The first
// frame is never throttled -- a tile that has nothing must show something at once.
//
// The readback maps a mip barely larger than the tile, so the CPU no longer sees
// the source resolution and this is cheap. What it now costs is one UI-thread
// repaint per tile per interval; a large grid, not a large desktop, is what would
// make this hurt.
constexpr auto kEmitInterval = std::chrono::milliseconds(100);

// A source that has produced nothing for this long has failed to start. It keeps
// retrying underneath; if a frame ever arrives the tile recovers on its own.
constexpr auto kFirstFrameGrace = std::chrono::milliseconds(2000);

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

CaptureSourceKey KeyOf(const ThumbnailTarget& t) {
    return CaptureSourceKey{t.is_monitor ? CaptureSourceKey::Kind::Monitor : CaptureSourceKey::Kind::Window,
                            t.native_id};
}

// Pulls a hub frame down to the CPU, small.
//
// The frame is copied into the top level of a mip chain, the GPU builds the rest,
// and only a level barely larger than the tile is mapped. A 4K desktop reaches
// the CPU as a few hundred kilobytes instead of thirty-three megabytes, which is
// the whole reason a tile can update at a live rate at all.
//
// Both textures are cached and only reallocated when the source size changes. If
// the mip chain cannot be built -- an adapter that refuses GENERATE_MIPS for this
// format -- the full-resolution path still works, just slowly.
class Readback {
  public:
    Readback(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11DeviceContext> context)
        : device_(std::move(device)), context_(std::move(context)) {
    }

    QImage ToImage(const HubFrame& frame, QSize desired) {
        if (!frame.texture || frame.width == 0 || frame.height == 0)
            return {};

        uint32_t level = 0;
        if (EnsureMipChain(frame.width, frame.height) && desired.width() > 0 && desired.height() > 0) {
            context_->CopySubresourceRegion(mip_.get(), 0, 0, 0, 0, frame.texture.get(), 0, nullptr);
            context_->GenerateMips(mip_srv_.get());
            level = ChooseMipLevel(frame.width, frame.height, mip_levels_, static_cast<uint32_t>(desired.width()),
                                   static_cast<uint32_t>(desired.height()));
        }

        const uint32_t width = mip_ ? MipExtent(frame.width, level) : frame.width;
        const uint32_t height = mip_ ? MipExtent(frame.height, level) : frame.height;
        if (!EnsureStaging(width, height))
            return {};

        if (mip_)
            context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, mip_.get(), level, nullptr);
        else
            context_->CopyResource(staging_.get(), frame.texture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return {};

        QImage view(static_cast<const uchar*>(mapped.pData), static_cast<int>(width), static_cast<int>(height),
                    static_cast<int>(mapped.RowPitch), QImage::Format_ARGB32);
        QImage copy = view.copy();
        context_->Unmap(staging_.get(), 0);
        return copy;
    }

  private:
    bool EnsureMipChain(uint32_t width, uint32_t height) {
        if (mip_ && mip_width_ == width && mip_height_ == height)
            return true;
        if (mip_failed_ && mip_width_ == width && mip_height_ == height)
            return false;

        mip_ = nullptr;
        mip_srv_ = nullptr;
        mip_width_ = width;
        mip_height_ = height;
        mip_failed_ = true;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 0; // full chain
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, mip_.put()))) {
            mip_ = nullptr;
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(mip_.get(), nullptr, mip_srv_.put()))) {
            mip_ = nullptr;
            mip_srv_ = nullptr;
            return false;
        }

        D3D11_TEXTURE2D_DESC actual{};
        mip_->GetDesc(&actual);
        mip_levels_ = actual.MipLevels;
        mip_failed_ = false;
        return true;
    }

    bool EnsureStaging(uint32_t width, uint32_t height) {
        if (staging_ && staging_width_ == width && staging_height_ == height)
            return true;

        staging_ = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging_.put()))) {
            staging_ = nullptr;
            return false;
        }
        staging_width_ = width;
        staging_height_ = height;
        return true;
    }

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;

    winrt::com_ptr<ID3D11Texture2D> mip_;
    winrt::com_ptr<ID3D11ShaderResourceView> mip_srv_;
    uint32_t mip_width_ = 0;
    uint32_t mip_height_ = 0;
    uint32_t mip_levels_ = 1;
    bool mip_failed_ = false;

    winrt::com_ptr<ID3D11Texture2D> staging_;
    uint32_t staging_width_ = 0;
    uint32_t staging_height_ = 0;
};

// One tile's view of its hub: what it last showed, and whether it has ever
// shown anything.
struct Tile {
    CaptureSubscription subscription;
    int target_index = -1;
    Clock::time_point subscribed_at{};
    Clock::time_point last_emit{};
    uint64_t last_generation = 0;
    bool failure_reported = false;
};

} // namespace

ThumbnailCapture::ThumbnailCapture(QObject* parent) : QObject(parent) {
    worker_ = std::jthread([this](std::stop_token st) { WorkerMain(st); });
}

ThumbnailCapture::~ThumbnailCapture() {
    if (worker_.joinable()) {
        worker_.request_stop();
        cv_.notify_one();
        worker_.join();
    }
}

void ThumbnailCapture::setTargets(std::vector<ThumbnailTarget> targets, QSize desired_size, int token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = Command{std::move(targets), desired_size, token};
    }
    cv_.notify_one();
}

void ThumbnailCapture::releaseAll() {
    setTargets({}, {}, 0);
}

void ThumbnailCapture::WorkerMain(std::stop_token stop_token) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_inited = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
    if (!com_inited)
        return;

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        if (FAILED(hr) || !device) {
            if (co != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    Readback readback(device, context);
    CaptureHubRegistry registry([device](const CaptureSourceKey& key) -> std::unique_ptr<HubSourceProducer> {
        return std::make_unique<WgcSourceProducer>(key, device);
    });

    // Every WGC producer, subscription and hub lives and dies on this thread.
    std::unordered_map<CaptureSourceKey, Tile, CaptureSourceKeyHash> tiles;
    QSize desired_size;
    int token = 0;

    auto post_ready = [this](int index, int tok, QImage image) {
        // Posted to `this`, not to the application: Qt then drops the event when
        // this object dies, instead of delivering into freed memory.
        QMetaObject::invokeMethod(
            this, [this, index, tok, img = std::move(image)]() mutable { emit thumbnailReady(index, tok, img); },
            Qt::QueuedConnection);
    };
    auto post_failed = [this](int index, int tok) {
        QMetaObject::invokeMethod(
            this, [this, index, tok]() { emit thumbnailFailed(index, tok); }, Qt::QueuedConnection);
    };

    Clock::time_point last_pump{};

    while (!stop_token.stop_requested()) {
        std::optional<Command> command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, kTick, [&] { return pending_.has_value() || stop_token.stop_requested(); });
            command = std::exchange(pending_, std::nullopt);
        }
        if (stop_token.stop_requested())
            break;

        const Clock::time_point now = Clock::now();

        if (command) {
            desired_size = command->desired_size;
            token = command->token;

            // Diff, do not rebuild: a target that survives the call keeps its
            // capture open and its held frame intact.
            std::unordered_map<CaptureSourceKey, ThumbnailTarget, CaptureSourceKeyHash> wanted;
            for (const ThumbnailTarget& t : command->targets) {
                if (t.native_id != 0)
                    wanted.emplace(KeyOf(t), t);
            }

            for (auto it = tiles.begin(); it != tiles.end();)
                it = wanted.count(it->first) ? std::next(it) : tiles.erase(it);

            for (const auto& [key, target] : wanted) {
                auto it = tiles.find(key);
                if (it == tiles.end()) {
                    Tile tile;
                    tile.target_index = target.target_index;
                    tile.subscribed_at = now;
                    // No push callback: a tile reads HeldFrame() on its own, far
                    // slower than the hub pumps, and a held frame is not pushed
                    // at all. Reacting to the callback would read back every
                    // frame the source produces.
                    tile.subscription = registry.Subscribe(key, [](const HubFrame&, recorder_core::HubFrameKind) {});
                    tiles.emplace(key, std::move(tile));
                } else {
                    // A new token means the caller wants to see the tile again;
                    // a still-warm hub answers immediately from its held frame.
                    it->second.target_index = target.target_index;
                    it->second.last_generation = 0;
                    it->second.failure_reported = false;
                }
            }
        }

        // WGC signals item.Closed through the apartment's message queue.
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (now - last_pump >= kPumpInterval) {
            registry.PumpAll();
            last_pump = now;
        }

        for (auto& [key, tile] : tiles) {
            if (tile.subscription.Frame() == recorder_core::HubFrameKind::None) {
                if (!tile.failure_reported && now - tile.subscribed_at >= kFirstFrameGrace) {
                    tile.failure_reported = true;
                    post_failed(tile.target_index, token);
                }
                continue;
            }

            const HubFrame frame = tile.subscription.HeldFrame();
            if (frame.generation == tile.last_generation)
                continue; // nothing new; the tile already shows this image
            if (tile.last_generation != 0 && now - tile.last_emit < kEmitInterval)
                continue;

            QImage image = readback.ToImage(frame, desired_size);
            if (image.isNull())
                continue;

            tile.last_generation = frame.generation;
            tile.last_emit = now;
            post_ready(tile.target_index, token, downscaleThumbnail(image, desired_size));
        }
    }

    // Subscriptions must die before the registry that owns their hubs, and every
    // WGC object must die on the apartment that created it.
    tiles.clear();

    if (co != RPC_E_CHANGED_MODE)
        CoUninitialize();
}

} // namespace exosnap
