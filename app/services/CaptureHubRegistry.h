#pragma once

// One hub per source key, created on the first consumer and destroyed with the
// last. The registry exists so that two consumers of the same window -- a picker
// tile and, later, a preview -- share one capture and therefore one held frame.
//
// A registry rather than one retargetable hub: the WGC hub genuinely serves
// several sources at once (a grid of picker tiles), and a map costs nothing over
// a special case while removing the "which source is it pointed at" state.
//
// Threading: single-threaded. Construct, subscribe, pump, unsubscribe and
// destroy on ONE thread -- the same thread the producers require, because a WGC
// frame pool belongs to the COM apartment that created it. Frame callbacks run
// inside PumpAll() on that thread and must not re-enter the registry.

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "services/CaptureSourceHub.h"
#include "services/CaptureSourceKey.h"

namespace exosnap {

class CaptureHubRegistry;

// A consumer's hold on one hub. Move-only; unsubscribes on destruction, after
// which the callback is never invoked again. The hub it names cannot be
// destroyed while a subscription to it lives, so the accessors are always valid.
class CaptureSubscription {
  public:
    CaptureSubscription() = default;
    ~CaptureSubscription();

    CaptureSubscription(const CaptureSubscription&) = delete;
    CaptureSubscription& operator=(const CaptureSubscription&) = delete;
    CaptureSubscription(CaptureSubscription&& other) noexcept;
    CaptureSubscription& operator=(CaptureSubscription&& other) noexcept;

    void Reset();

    [[nodiscard]] explicit operator bool() const noexcept {
        return registry_ != nullptr;
    }

    // None while the source has never produced; Held once it has stopped; Live
    // while it produces. An empty subscription reports None.
    [[nodiscard]] exosnap::engine::HubFrameKind Frame() const;

    // The last good frame, whatever the source is doing now. Empty texture while
    // Frame() is None.
    [[nodiscard]] HubFrame HeldFrame() const;

  private:
    friend class CaptureHubRegistry;
    CaptureSubscription(CaptureHubRegistry* registry, CaptureSourceKey key, uint64_t token)
        : registry_(registry), key_(key), token_(token) {
    }

    CaptureHubRegistry* registry_ = nullptr;
    CaptureSourceKey key_{};
    uint64_t token_ = 0;
};

class CaptureHubRegistry {
  public:
    // Builds the capture underneath a hub. Injected, so the registry and its
    // consumers are testable without WGC, D3D or a GPU.
    using ProducerFactory = std::function<std::unique_ptr<HubSourceProducer>(const CaptureSourceKey&)>;

    explicit CaptureHubRegistry(ProducerFactory factory);
    ~CaptureHubRegistry();

    CaptureHubRegistry(const CaptureHubRegistry&) = delete;
    CaptureHubRegistry& operator=(const CaptureHubRegistry&) = delete;

    [[nodiscard]] CaptureSubscription Subscribe(const CaptureSourceKey& key, CaptureSourceHub::FrameCallback cb);

    // One step of every live hub: poll, or retry a reopen. Cheap for a hub with
    // nothing to own.
    void PumpAll();

    // The recording engine is about to capture / has released this source. A
    // display may only be duplicated once per process, so the hub's capture is
    // closed before the lease is granted and reopened on return. Returns false
    // for a key without a live hub (nothing to release — the engine may open
    // its capture directly). Consumers stay subscribed and see the held frame.
    bool RequestLease(const CaptureSourceKey& key);
    void ReturnLease(const CaptureSourceKey& key);

    [[nodiscard]] size_t HubCountForTest() const {
        return hubs_.size();
    }

  private:
    friend class CaptureSubscription;
    void Unsubscribe(const CaptureSourceKey& key, uint64_t token);
    [[nodiscard]] const CaptureSourceHub* Find(const CaptureSourceKey& key) const;

    ProducerFactory factory_;
    std::unordered_map<CaptureSourceKey, std::unique_ptr<CaptureSourceHub>, CaptureSourceKeyHash> hubs_;
};

} // namespace exosnap
