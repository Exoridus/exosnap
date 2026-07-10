#include "services/CaptureHubRegistry.h"

#include <utility>

namespace exosnap {

using recorder_core::HubFrameKind;

CaptureSubscription::~CaptureSubscription() {
    Reset();
}

CaptureSubscription::CaptureSubscription(CaptureSubscription&& other) noexcept
    : registry_(other.registry_), key_(other.key_), token_(other.token_) {
    other.registry_ = nullptr;
}

CaptureSubscription& CaptureSubscription::operator=(CaptureSubscription&& other) noexcept {
    if (this != &other) {
        Reset();
        registry_ = other.registry_;
        key_ = other.key_;
        token_ = other.token_;
        other.registry_ = nullptr;
    }
    return *this;
}

void CaptureSubscription::Reset() {
    if (!registry_)
        return;
    // Clear first: Unsubscribe may destroy the hub, and a second Reset must not
    // reach a key that is gone.
    CaptureHubRegistry* registry = std::exchange(registry_, nullptr);
    registry->Unsubscribe(key_, token_);
}

HubFrameKind CaptureSubscription::Frame() const {
    if (!registry_)
        return HubFrameKind::None;
    const CaptureSourceHub* hub = registry_->Find(key_);
    return hub ? hub->Frame() : HubFrameKind::None;
}

HubFrame CaptureSubscription::HeldFrame() const {
    if (!registry_)
        return {};
    const CaptureSourceHub* hub = registry_->Find(key_);
    return hub ? hub->HeldFrame() : HubFrame{};
}

CaptureHubRegistry::CaptureHubRegistry(ProducerFactory factory) : factory_(std::move(factory)) {
}

CaptureHubRegistry::~CaptureHubRegistry() = default;

const CaptureSourceHub* CaptureHubRegistry::Find(const CaptureSourceKey& key) const {
    const auto it = hubs_.find(key);
    return it == hubs_.end() ? nullptr : it->second.get();
}

CaptureSubscription CaptureHubRegistry::Subscribe(const CaptureSourceKey& key, CaptureSourceHub::FrameCallback cb) {
    auto it = hubs_.find(key);
    if (it == hubs_.end()) {
        auto hub = std::make_unique<CaptureSourceHub>(factory_(key));
        it = hubs_.emplace(key, std::move(hub)).first;
    }
    // Subscribing opens the capture when this is the first consumer. A producer
    // that refuses to open leaves the hub holding and retrying; that is not a
    // subscription failure, and the consumer still gets its frames on recovery.
    const uint64_t token = it->second->Subscribe(std::move(cb));
    return CaptureSubscription(this, key, token);
}

void CaptureHubRegistry::Unsubscribe(const CaptureSourceKey& key, uint64_t token) {
    const auto it = hubs_.find(key);
    if (it == hubs_.end())
        return;

    it->second->Unsubscribe(token);

    // The last consumer left: the capture is already closed by the hub's policy,
    // and nothing must keep polling a source nobody watches.
    if (it->second->ConsumerCount() == 0)
        hubs_.erase(it);
}

void CaptureHubRegistry::PumpAll() {
    // Pumping cannot add or remove hubs: callbacks may not re-enter the
    // registry, so iterating the map directly is safe.
    for (auto& [key, hub] : hubs_)
        hub->Pump();
}

} // namespace exosnap
