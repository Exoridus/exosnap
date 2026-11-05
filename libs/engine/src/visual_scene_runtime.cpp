#include <exosnap/engine/visual_scene_runtime.h>

#include "gamepad_frame_sampler.h"
#include "visual_scene_runtime_storage.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <exosnap/engine/gamepad_sprite_data.h>
#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/visual_frame.h>
#include <exosnap/engine/visual_scene.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <new>
#include <utility>

namespace exosnap::engine {
namespace {

using namespace detail;

EngineError Failure(EngineErrorCode code, const char* context) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Runtime;
    error.context = context;
    error.retryability = code == EngineErrorCode::Stalled ? Retryability::Transient : Retryability::Never;
    return error;
}

bool Valid(VisualAddress address) noexcept {
    return address.runtime.value != 0 &&
           std::ranges::any_of(address.source.bytes, [](uint8_t value) { return value != 0; });
}

bool Add(uint64_t first, uint64_t second, uint64_t& result) noexcept {
    if (second > UINT64_MAX - first)
        return false;
    result = first + second;
    return true;
}

bool Multiply(uint64_t first, uint64_t second, uint64_t& result) noexcept {
    if (second != 0 && first > UINT64_MAX / second)
        return false;
    result = first * second;
    return true;
}

bool Fits(const VisualRuntimeStorage& storage, uint64_t bytes) noexcept {
    uint64_t total;
    return Add(storage.reserved_bytes, bytes, total) &&
           (!storage.policy.cpu_payload_limit_bytes || total <= *storage.policy.cpu_payload_limit_bytes);
}

uint64_t Next(uint64_t& next) noexcept {
    const auto value = next;
    next = value == UINT64_MAX ? 0 : value + 1;
    return value;
}

VisualSourceStorage* Find(VisualRuntimeStorage& storage, VisualAddress address) noexcept {
    const auto found = storage.sources.find(address);
    return found == storage.sources.end() ? nullptr : found->second.get();
}

bool PoolIdle(const CpuImagePool& pool) noexcept {
    return std::ranges::all_of(pool.frames,
                               [](const auto& slot) { return slot->pins.load(std::memory_order_acquire) == 0; });
}

long HostAtlasOwners(const VisualRuntimeStorage& storage, const SpriteAtlas* atlas) noexcept {
    long owners = 0;
    for (const auto& [address, source] : storage.sources) {
        for (const auto& data : source->gamepads) {
            if (!data || data->sprites->atlas.get() != atlas)
                continue;
            bool counted = false;
            for (const auto& [other_address, other_source] : storage.sources) {
                counted = counted || std::ranges::any_of(other_source->gamepads, [&](const auto& other) {
                              return other && other->sprites.get() == data->sprites.get() &&
                                     std::less<const GamepadResource*>{}(other.get(), data.get());
                          });
            }
            if (!counted)
                ++owners;
        }
    }
    return owners;
}

uint64_t SceneMultiplicity(const SceneSlot& scene, const GamepadResource& resource) noexcept {
    uint64_t count = 0;
    for (const auto& item : scene.items)
        if (scene.bindings[item.binding_index].resource_id == resource.id)
            ++count;
    return count;
}

void PruneSources(VisualRuntimeStorage& storage) {
    std::erase_if(storage.sources, [&](const auto& entry) {
        const auto& [address, source] = entry;
        if (!source->revoked || source->image_preparing || source->retiring_resources != 0 || !source->pools.empty() ||
            !source->gamepads.empty())
            return false;
        for (const auto& [id, consumer] : storage.consumers)
            for (const auto& image : consumer->images)
                if (image->address == address && image->read.pins.load(std::memory_order_acquire) != 0)
                    return false;
        for (auto& [id, consumer] : storage.consumers)
            std::erase_if(consumer->images, [&](const auto& image) { return image->address == address; });
        return true;
    });
}

void Collect(VisualRuntimeStorage& storage, std::unique_lock<std::mutex>& lock) {
    std::vector<std::pair<VisualSourceStorage*, std::unique_ptr<CpuImagePool>>> pools;
    std::vector<std::pair<VisualSourceStorage*, std::shared_ptr<GamepadResource>>> gamepads;
    size_t pool_count = 0;
    size_t gamepad_count = 0;
    for (const auto& [address, source] : storage.sources) {
        pool_count += source->pools.size();
        gamepad_count += source->gamepads.size();
    }
    try {
        pools.reserve(pool_count);
        gamepads.reserve(gamepad_count);
    } catch (const std::bad_alloc&) {
        return;
    }
    for (auto& scene : storage.scenes) {
        if (scene.get() != storage.scene && scene->pins.load(std::memory_order_acquire) == 0) {
            scene->gamepads.clear();
            scene->bindings.clear();
            scene->items.clear();
        }
    }
    uint64_t bytes = 0;
    for (auto& [address, source] : storage.sources) {
        for (auto& pool : source->pools) {
            if (pool.get() == source->active_pool || pool.get() == source->accepted_pool || !PoolIdle(*pool))
                continue;
            bytes += pool->bytes;
            ++source->retiring_resources;
            pools.emplace_back(source.get(), std::move(pool));
        }
        std::erase(source->pools, nullptr);
        for (auto& data : source->gamepads) {
            const auto host_owners = HostAtlasOwners(storage, data->sprites->atlas.get());
            bool other_host_resource = false;
            for (const auto& [other_address, other_source] : storage.sources)
                other_host_resource =
                    other_host_resource || std::ranges::any_of(other_source->gamepads, [&](const auto& other) {
                        return other && other.get() != data.get() &&
                               other->sprites->atlas.get() == data->sprites->atlas.get();
                    });
            if (data.get() == source->active_gamepad || data.use_count() != 1 ||
                (!other_host_resource && data->sprites->atlas.use_count() > host_owners))
                continue;
            bytes += data->bytes;
            ++source->retiring_resources;
            gamepads.emplace_back(source.get(), std::move(data));
        }
        std::erase(source->gamepads, nullptr);
        for (auto& data : source->gamepads) {
            uint64_t instances = 1;
            for (const auto& scene : storage.scenes)
                instances = std::max(instances, SceneMultiplicity(*scene, *data));
            const uint64_t released =
                (data->scratch_instances - instances) * data->scratch_per_consumer * storage.consumers.size();
            data->scratch_instances = instances;
            data->bytes -= released;
            storage.reserved_bytes -= released;
        }
    }
    if (pools.empty() && gamepads.empty()) {
        PruneSources(storage);
        return;
    }
    // Retiring generations stay charged until their payload destructors finish.
    // Their source records cannot be pruned during this unlocked interval.
    lock.unlock();
    for (auto& entry : pools)
        entry.second.reset();
    for (auto& entry : gamepads)
        entry.second.reset();
    lock.lock();
    storage.reserved_bytes -= bytes;
    for (const auto& entry : pools)
        --entry.first->retiring_resources;
    for (const auto& entry : gamepads)
        --entry.first->retiring_resources;
    PruneSources(storage);
}
uint64_t ConsumerScratch(const VisualRuntimeStorage& storage) noexcept {
    uint64_t bytes = 0;
    for (const auto& [address, source] : storage.sources) {
        for (const auto& data : source->gamepads) {
            uint64_t scratch;
            if (!Multiply(data->scratch_per_consumer, data->scratch_instances, scratch) || !Add(bytes, scratch, bytes))
                return UINT64_MAX;
        }
    }
    return bytes;
}

bool HasGenerationCapacity(const VisualRuntimeStorage& storage, VisualSourceId id) noexcept {
    uint64_t count = 0;
    for (const auto& [address, source] : storage.sources) {
        if (address.source != id)
            continue;
        // Each allocation has one owning entry; scene references do not count twice.
        count += source->pools.size() + source->gamepads.size() + source->retiring_resources +
                 (source->image_preparing ? 1u : 0u);
        if (count >= storage.policy.max_resource_generations)
            return false;
    }
    return count < storage.policy.max_resource_generations;
}

bool ValidItem(const PreparedSceneItem& item) noexcept {
    const auto& p = item.properties.placement;
    const auto& c = item.chroma_key;
    return std::ranges::any_of(item.id.bytes, [](uint8_t value) { return value != 0; }) && std::isfinite(p.x) &&
           std::isfinite(p.y) && std::isfinite(p.w) && std::isfinite(p.h) && p.w > 0 && p.h > 0 &&
           std::isfinite(item.properties.opacity) && item.properties.opacity >= 0 && item.properties.opacity <= 1 &&
           std::isfinite(c.tolerance) && std::isfinite(c.softness) && std::isfinite(c.spill_reduction);
}

} // namespace

CpuFrameWriteLease::~CpuFrameWriteLease() {
    Reset();
}
CpuFrameWriteLease::CpuFrameWriteLease(CpuFrameWriteLease&& other) noexcept {
    *this = std::move(other);
}
CpuFrameWriteLease& CpuFrameWriteLease::operator=(CpuFrameWriteLease&& other) noexcept {
    if (this != &other) {
        Reset();
        storage_ = std::move(other.storage_);
        pool_ = std::exchange(other.pool_, nullptr);
        slot_ = std::exchange(other.slot_, nullptr);
    }
    return *this;
}
std::span<uint8_t> CpuFrameWriteLease::Pixels() noexcept {
    return slot_ ? std::span<uint8_t>(slot_->image.pixels) : std::span<uint8_t>{};
}
const VisualFrameStamp& CpuFrameWriteLease::Stamp() const noexcept {
    assert(slot_);
    return slot_->stamp;
}
const CpuImageLayout& CpuFrameWriteLease::Layout() const noexcept {
    assert(pool_);
    return pool_->layout;
}
void CpuFrameWriteLease::Reset() noexcept {
    if (slot_)
        slot_->pins.store(0, std::memory_order_release);
    slot_ = nullptr;
    pool_ = nullptr;
    storage_.reset();
}

VisualReadLease::~VisualReadLease() {
    Reset();
}
VisualReadLease::VisualReadLease(VisualReadLease&& other) noexcept {
    *this = std::move(other);
}
VisualReadLease& VisualReadLease::operator=(VisualReadLease&& other) noexcept {
    if (this != &other) {
        Reset();
        reader_ = std::exchange(other.reader_, nullptr);
        pool_ = std::exchange(other.pool_, nullptr);
        slot_ = std::exchange(other.slot_, nullptr);
    }
    return *this;
}
const WebcamFrameSnapshot& VisualReadLease::Image() const noexcept {
    assert(slot_);
    return slot_->image;
}
uint64_t VisualReadLease::ResourceId() const noexcept {
    return pool_ ? pool_->id : 0;
}
const VisualFrameStamp& VisualReadLease::Stamp() const noexcept {
    assert(slot_);
    return slot_->stamp;
}
void VisualReadLease::Reset() noexcept {
    if (slot_)
        slot_->pins.fetch_sub(1, std::memory_order_release);
    if (reader_)
        reader_->pins.fetch_sub(1, std::memory_order_release);
    reader_ = nullptr;
    slot_ = nullptr;
    pool_ = nullptr;
}

SceneReadLease::~SceneReadLease() {
    Reset();
}
SceneReadLease::SceneReadLease(SceneReadLease&& other) noexcept {
    *this = std::move(other);
}
SceneReadLease& SceneReadLease::operator=(SceneReadLease&& other) noexcept {
    if (this != &other) {
        Reset();
        reader_ = std::exchange(other.reader_, nullptr);
        slot_ = std::exchange(other.slot_, nullptr);
    }
    return *this;
}
std::span<const PreparedSceneItem> SceneReadLease::Items() const noexcept {
    return slot_ ? std::span<const PreparedSceneItem>(slot_->items) : std::span<const PreparedSceneItem>{};
}
std::span<const PreparedSceneBinding> SceneReadLease::Bindings() const noexcept {
    return slot_ ? std::span<const PreparedSceneBinding>(slot_->bindings) : std::span<const PreparedSceneBinding>{};
}
ConfigRevision SceneReadLease::Revision() const noexcept {
    return slot_ ? slot_->revision : ConfigRevision{};
}
uint64_t SceneReadLease::PublicationSequence() const noexcept {
    return slot_ ? slot_->sequence : 0;
}
void SceneReadLease::Reset() noexcept {
    if (slot_)
        slot_->pins.fetch_sub(1, std::memory_order_release);
    if (reader_)
        reader_->pins.fetch_sub(1, std::memory_order_release);
    reader_ = nullptr;
    slot_ = nullptr;
}

VisualSceneRuntime::VisualSceneRuntime(VisualBudgetPolicy policy) : storage_(std::make_shared<VisualRuntimeStorage>()) {
    storage_->policy = policy;
    for (size_t index = 0; index < 2; ++index)
        storage_->scenes.push_back(std::make_unique<SceneSlot>());
}
VisualSceneRuntime::~VisualSceneRuntime() = default;

Outcome<VisualConsumerId> VisualSceneRuntime::RegisterConsumer() {
    std::unique_lock lock(storage_->mutex);
    if (storage_->next_consumer == 0)
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "consumer IDs");
    const uint64_t scratch = ConsumerScratch(*storage_);
    if (scratch == UINT64_MAX || !Fits(*storage_, scratch))
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "reader scratch");
    try {
        auto consumer = std::make_unique<VisualConsumerState>();
        for (const auto& [address, source] : storage_->sources) {
            auto image = std::make_unique<ImageConsumerState>();
            image->address = address;
            consumer->images.push_back(std::move(image));
        }
        auto scene = std::make_unique<SceneSlot>();
        storage_->scenes.reserve(storage_->scenes.size() + 1);
        const auto id = storage_->next_consumer;
        storage_->consumers.emplace(id, std::move(consumer));
        storage_->scenes.push_back(std::move(scene));
        storage_->next_consumer = id == UINT32_MAX ? 0 : id + 1;
        storage_->reserved_bytes += scratch;
        for (auto& [address, source] : storage_->sources)
            for (auto& data : source->gamepads)
                data->bytes += data->scratch_per_consumer * data->scratch_instances;
        return id;
    } catch (const std::bad_alloc&) {
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "reader storage");
    }
}

void VisualSceneRuntime::UnregisterConsumer(VisualConsumerId id) {
    std::unique_lock lock(storage_->mutex);
    const auto found = storage_->consumers.find(id);
    if (found == storage_->consumers.end() || found->second->scene.pins.load(std::memory_order_acquire) != 0 ||
        std::ranges::any_of(found->second->images,
                            [](const auto& image) { return image->read.pins.load(std::memory_order_acquire) != 0; }))
        return;
    storage_->reserved_bytes -= ConsumerScratch(*storage_);
    for (auto& [address, source] : storage_->sources)
        for (auto& data : source->gamepads)
            data->bytes -= data->scratch_per_consumer * data->scratch_instances;
    storage_->consumers.erase(found);
    const auto removable = std::ranges::find_if(storage_->scenes, [&](const auto& scene) {
        return scene.get() != storage_->scene && scene->pins.load(std::memory_order_acquire) == 0;
    });
    if (removable != storage_->scenes.end())
        storage_->scenes.erase(removable);
    Collect(*storage_, lock);
}

Outcome<VisualPublicationWriter> VisualSceneRuntime::CreateSource(VisualAddress address) {
    std::unique_lock lock(storage_->mutex);
    if (!Valid(address) || address.runtime.value <= storage_->last_runtime_generation)
        return Failure(EngineErrorCode::InvalidConfig, "source address");
    try {
        auto source = std::make_unique<VisualSourceStorage>();
        source->token.address = address;
        std::vector<std::unique_ptr<ImageConsumerState>> readers;
        readers.reserve(storage_->consumers.size());
        for (auto& [id, consumer] : storage_->consumers) {
            auto reader = std::make_unique<ImageConsumerState>();
            reader->address = address;
            readers.push_back(std::move(reader));
            consumer->images.reserve(consumer->images.size() + 1);
        }
        storage_->sources.emplace(address, std::move(source));
        size_t index = 0;
        for (auto& [id, consumer] : storage_->consumers)
            consumer->images.push_back(std::move(readers[index++]));
        storage_->last_runtime_generation = address.runtime.value;
        VisualPublicationWriter writer;
        writer.storage_ = storage_;
        writer.address_ = address;
        return writer;
    } catch (const std::bad_alloc&) {
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "source storage");
    }
}

void VisualSceneRuntime::SetRequest(const SourceToken& token, bool allowed, HoldPolicy hold) noexcept {
    std::unique_lock lock(storage_->mutex);
    auto* source = Find(*storage_, token.address);
    if (!source || source->revoked || token.request.value == 0 || token.request < source->token.request ||
        (token == source->token && allowed && !source->publish_allowed))
        return;
    source->token = token;
    source->publish_allowed = allowed;
    if (hold == HoldPolicy::ClearAccepted) {
        source->accepted = nullptr;
        source->accepted_pool = nullptr;
    }
}

Outcome<std::monostate> VisualPublicationWriter::PrepareImage(const SourceToken& token, const CpuImageLayout& layout) {
    if (!storage_ || token.address != address_)
        return Failure(EngineErrorCode::InvalidConfig, "image writer");
    const auto payload = WebcamFramePayloadBytes(layout.format, layout.width, layout.height);
    if (payload == 0)
        return Failure(EngineErrorCode::InvalidConfig, "image layout");
    std::unique_ptr<CpuImagePool> pool;
    std::unique_lock lock(storage_->mutex);
    auto* source = Find(*storage_, address_);
    if (!source || source->revoked || token != source->token || !source->publish_allowed)
        return Failure(EngineErrorCode::StaleRevision, "image request");
    if (!source->gamepads.empty())
        return Failure(EngineErrorCode::InvalidConfig, "source kind");
    if (source->image_preparing)
        return Failure(EngineErrorCode::Stalled, "image preparing");
    for (const auto& existing : source->pools) {
        if (existing->layout == layout) {
            source->active_pool = existing.get();
            return std::monostate{};
        }
    }
    if (storage_->policy.camera_slots == 0 || storage_->policy.max_resource_generations == 0)
        return Failure(EngineErrorCode::InvalidConfig, "image policy");
    if (source->active_pool != source->accepted_pool && source->active_pool && PoolIdle(*source->active_pool))
        source->active_pool = source->accepted_pool;
    Collect(*storage_, lock);
    source = Find(*storage_, address_);
    if (!source || source->revoked || token != source->token || !source->publish_allowed)
        return Failure(EngineErrorCode::StaleRevision, "image request");
    if (!source->gamepads.empty())
        return Failure(EngineErrorCode::InvalidConfig, "source kind");
    if (source->image_preparing)
        return Failure(EngineErrorCode::Stalled, "image preparing");
    for (const auto& existing : source->pools) {
        if (existing->layout == layout) {
            source->active_pool = existing.get();
            return std::monostate{};
        }
    }
    if (!HasGenerationCapacity(*storage_, address_.source))
        return Failure(EngineErrorCode::Stalled, "image pools busy");
    uint64_t bytes;
    if (storage_->next_resource_id == 0 || !Multiply(payload, storage_->policy.camera_slots, bytes) ||
        !Fits(*storage_, bytes))
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "image pool");
    source->image_preparing = true;
    storage_->reserved_bytes += bytes;
    bool reserved = true;
    const auto release_reservation = [&] {
        if (reserved) {
            source->image_preparing = false;
            storage_->reserved_bytes -= bytes;
            reserved = false;
        }
    };
    const uint32_t frame_count = storage_->policy.camera_slots;
    lock.unlock();
    try {
        pool = std::make_unique<CpuImagePool>();
        pool->layout = layout;
        pool->bytes = bytes;
        pool->frames.reserve(frame_count);
        for (uint32_t index = 0; index < frame_count; ++index) {
            auto slot = std::make_unique<CpuFrameSlot>();
            slot->image.width = layout.width;
            slot->image.height = layout.height;
            slot->image.format = layout.format;
            slot->image.matrix = layout.matrix;
            slot->image.range = layout.range;
            slot->image.pixels.resize(payload);
            pool->frames.push_back(std::move(slot));
        }
        lock.lock();
        if (source->revoked || source->token != token || !source->publish_allowed) {
            lock.unlock();
            pool.reset();
            lock.lock();
            release_reservation();
            return Failure(EngineErrorCode::StaleRevision, "image request");
        }
        if (storage_->next_resource_id == 0) {
            lock.unlock();
            pool.reset();
            lock.lock();
            release_reservation();
            return Failure(EngineErrorCode::ResourceBudgetExceeded, "image pool ID");
        }
        source->pools.reserve(source->pools.size() + 1);
        pool->id = Next(storage_->next_resource_id);
        source->active_pool = pool.get();
        source->pools.push_back(std::move(pool));
        source->image_preparing = false;
        reserved = false;
        return std::monostate{};
    } catch (const std::exception&) {
        if (lock.owns_lock())
            lock.unlock();
        pool.reset();
        lock.lock();
        release_reservation();
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "image pool");
    }
}

std::optional<CpuFrameWriteLease> VisualPublicationWriter::TryBeginCpuFrame(const VisualFrameStamp& stamp) noexcept {
    if (!storage_ || stamp.token.address != address_ || stamp.sequence == 0)
        return std::nullopt;
    const std::unique_lock lock(storage_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        storage_->dropped_publications.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    auto* source = Find(*storage_, address_);
    if (!source || source->revoked || !source->publish_allowed || source->token != stamp.token || !source->active_pool)
        return std::nullopt;
    for (const auto& slot : source->active_pool->frames) {
        if (slot.get() == source->accepted || slot->pins.load(std::memory_order_acquire) != 0)
            continue;
        slot->pins.store(kWritingSlot, std::memory_order_release);
        slot->stamp = stamp;
        slot->image.generation = stamp.sequence;
        CpuFrameWriteLease lease;
        lease.storage_ = storage_;
        lease.pool_ = source->active_pool;
        lease.slot_ = slot.get();
        return lease;
    }
    storage_->dropped_publications.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

PublishResult VisualPublicationWriter::Publish(CpuFrameWriteLease&& lease) noexcept {
    CpuFrameWriteLease frame = std::move(lease);
    if (!storage_ || !frame || frame.storage_ != storage_ || frame.Stamp().token.address != address_)
        return PublishResult::Invalid;
    const std::unique_lock lock(storage_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        storage_->dropped_publications.fetch_add(1, std::memory_order_relaxed);
        return PublishResult::NoFreeSlot;
    }
    auto* source = Find(*storage_, address_);
    if (!source || source->revoked)
        return PublishResult::Revoked;
    if (!source->publish_allowed || source->token != frame.Stamp().token || source->active_pool != frame.pool_ ||
        (source->accepted && source->accepted->stamp.token == frame.Stamp().token &&
         source->accepted->stamp.sequence >= frame.Stamp().sequence))
        return PublishResult::Superseded;
    source->accepted = frame.slot_;
    source->accepted_pool = frame.pool_;
    frame.slot_->pins.store(0, std::memory_order_release);
    frame.slot_ = nullptr;
    frame.pool_ = nullptr;
    storage_->accepted_publications.fetch_add(1, std::memory_order_relaxed);
    return PublishResult::Accepted;
}

ReadResult VisualSceneRuntime::TryReadImage(VisualConsumerId id, VisualAddress address,
                                            VisualReadLease& result) noexcept {
    if (result)
        return ReadResult::Busy;
    const std::unique_lock lock(storage_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        storage_->busy_reads.fetch_add(1, std::memory_order_relaxed);
        return ReadResult::Busy;
    }
    auto* source = Find(*storage_, address);
    const auto consumer = storage_->consumers.find(id);
    if (!source || source->revoked || consumer == storage_->consumers.end())
        return ReadResult::Revoked;
    if (!source->publish_allowed || !source->accepted)
        return ReadResult::NotReady;
    const auto found =
        std::ranges::find_if(consumer->second->images, [&](const auto& image) { return image->address == address; });
    if (found == consumer->second->images.end())
        return ReadResult::Revoked;
    auto& reader = (*found)->read;
    if (reader.pins.load(std::memory_order_acquire) != 0)
        return ReadResult::Busy;
    if (reader.sequence == source->accepted->stamp.sequence && reader.token == source->accepted->stamp.token &&
        reader.resource_id == source->accepted_pool->id)
        return ReadResult::Unchanged;
    source->accepted->pins.fetch_add(1, std::memory_order_acquire);
    reader.pins.store(1, std::memory_order_release);
    reader.sequence = source->accepted->stamp.sequence;
    reader.token = source->accepted->stamp.token;
    reader.resource_id = source->accepted_pool->id;
    result.reader_ = &reader;
    result.pool_ = source->accepted_pool;
    result.slot_ = source->accepted;
    return ReadResult::Updated;
}

Outcome<std::monostate> VisualSceneRuntime::InvalidateImage(VisualConsumerId id, VisualAddress address) {
    const std::scoped_lock lock(storage_->mutex);
    const auto consumer = storage_->consumers.find(id);
    if (consumer == storage_->consumers.end())
        return Failure(EngineErrorCode::InvalidConfig, "image consumer");
    const auto found =
        std::ranges::find_if(consumer->second->images, [&](const auto& image) { return image->address == address; });
    if (found == consumer->second->images.end())
        return Failure(EngineErrorCode::InvalidConfig, "image source");
    auto& reader = (*found)->read;
    if (reader.pins.load(std::memory_order_acquire) != 0)
        return Failure(EngineErrorCode::Stalled, "image reader pinned");
    reader.sequence = 0;
    reader.resource_id = 0;
    reader.token = {};
    return std::monostate{};
}

Outcome<ConfigRevision> VisualSceneRuntime::PublishScene(const PreparedSceneDescription& description) {
    SceneSlot staged;
    std::unique_lock lock(storage_->mutex);
    if (description.revision.value == 0 || description.revision < storage_->accepted_revision)
        return Failure(EngineErrorCode::StaleRevision, "scene revision");
    if (storage_->next_scene_sequence == 0)
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "scene serial");
    for (size_t index = 0; index < description.items.size(); ++index) {
        const auto& item = description.items[index];
        if (!ValidItem(item) || item.binding_index >= description.binding_sources.size() ||
            item.source != description.binding_sources[item.binding_index])
            return Failure(EngineErrorCode::InvalidConfig, "scene binding");
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (description.items[earlier].id == item.id)
                return Failure(EngineErrorCode::InvalidConfig, "scene item ID");
    }
    const auto available = std::ranges::find_if(storage_->scenes, [&](const auto& scene) {
        return scene.get() != storage_->scene && scene->pins.load(std::memory_order_acquire) == 0;
    });
    if (available == storage_->scenes.end())
        return Failure(EngineErrorCode::Stalled, "scene slots busy");
    try {
        staged.items = description.items;
        staged.bindings.reserve(description.binding_sources.size());
        staged.gamepads.reserve(description.binding_sources.size());
        for (const auto& address : description.binding_sources) {
            auto* source = Find(*storage_, address);
            if (!source || source->revoked)
                return Failure(EngineErrorCode::InvalidConfig, "scene source");
            PreparedSceneBinding binding;
            binding.token = source->token;
            if (source->active_gamepad) {
                const auto& data = *source->active_gamepad;
                binding.token = data.token;
                binding.resource_id = data.id;
                binding.content = GamepadBinding{data.sprites.get(), &data.history};
                const auto owner = std::ranges::find_if(
                    source->gamepads, [&](const auto& candidate) { return candidate.get() == &data; });
                staged.gamepads.push_back(*owner);
            } else if (const auto* pool = source->accepted_pool ? source->accepted_pool : source->active_pool) {
                binding.resource_id = pool->id;
                binding.content = CpuImageBinding{pool->layout};
                if (source->accepted)
                    binding.token = source->accepted->stamp.token;
            }
            staged.bindings.push_back(binding);
        }
        uint64_t additional_scratch = 0;
        for (const auto& [address, source] : storage_->sources) {
            for (const auto& data : source->gamepads) {
                const auto instances = std::max(data->scratch_instances, SceneMultiplicity(staged, *data));
                uint64_t bytes;
                if (!Multiply(instances - data->scratch_instances, data->scratch_per_consumer, bytes) ||
                    !Multiply(bytes, storage_->consumers.size(), bytes) ||
                    !Add(additional_scratch, bytes, additional_scratch))
                    return Failure(EngineErrorCode::ResourceBudgetExceeded, "scene scratch");
            }
        }
        if (!Fits(*storage_, additional_scratch))
            return Failure(EngineErrorCode::ResourceBudgetExceeded, "scene scratch");
        for (auto& [address, source] : storage_->sources) {
            for (auto& data : source->gamepads) {
                const auto instances = std::max(data->scratch_instances, SceneMultiplicity(staged, *data));
                data->bytes +=
                    (instances - data->scratch_instances) * data->scratch_per_consumer * storage_->consumers.size();
                data->scratch_instances = instances;
            }
        }
        storage_->reserved_bytes += additional_scratch;
        auto& slot = **available;
        slot.items.swap(staged.items);
        slot.bindings.swap(staged.bindings);
        slot.gamepads.swap(staged.gamepads);
        slot.revision = description.revision;
        slot.sequence = Next(storage_->next_scene_sequence);
        storage_->scene = &slot;
        storage_->accepted_revision = description.revision;
        const auto sequence = slot.sequence;
        lock.unlock();
        auto published = storage_->scene_sequence.load(std::memory_order_relaxed);
        while (published < sequence && !storage_->scene_sequence.compare_exchange_weak(
                                           published, sequence, std::memory_order_release, std::memory_order_relaxed)) {
        }
        return description.revision;
    } catch (const std::bad_alloc&) {
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "scene storage");
    }
}

uint64_t VisualSceneRuntime::PublishedSceneSequence() const noexcept {
    return storage_->scene_sequence.load(std::memory_order_acquire);
}

ReadResult VisualSceneRuntime::TryReadScene(VisualConsumerId id, SceneReadLease& result) noexcept {
    if (result)
        return ReadResult::Busy;
    const std::unique_lock lock(storage_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        storage_->busy_reads.fetch_add(1, std::memory_order_relaxed);
        return ReadResult::Busy;
    }
    const auto consumer = storage_->consumers.find(id);
    if (consumer == storage_->consumers.end())
        return ReadResult::Revoked;
    if (!storage_->scene)
        return ReadResult::NotReady;
    // The slot becomes visible under the mutex before its serial is advertised.
    // Returning it early could make a consumer mistake the older serial for an edit.
    if (storage_->scene->sequence > storage_->scene_sequence.load(std::memory_order_acquire))
        return ReadResult::Busy;
    auto& reader = consumer->second->scene;
    if (reader.pins.load(std::memory_order_acquire) != 0)
        return ReadResult::Busy;
    if (reader.sequence == storage_->scene->sequence)
        return ReadResult::Unchanged;
    storage_->scene->pins.fetch_add(1, std::memory_order_acquire);
    reader.pins.store(1, std::memory_order_release);
    reader.sequence = storage_->scene->sequence;
    result.reader_ = &reader;
    result.slot_ = storage_->scene;
    return ReadResult::Updated;
}

ReadResult VisualSceneRuntime::CheckBinding(const PreparedSceneBinding& binding) noexcept {
    const std::unique_lock lock(storage_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        storage_->busy_reads.fetch_add(1, std::memory_order_relaxed);
        return ReadResult::Busy;
    }
    auto* source = Find(*storage_, binding.token.address);
    if (!source || source->revoked)
        return ReadResult::Revoked;
    if (!source->publish_allowed || binding.resource_id == 0)
        return ReadResult::NotReady;
    if (std::holds_alternative<CpuImageBinding>(binding.content)) {
        return source->accepted && source->accepted_pool->id == binding.resource_id &&
                       source->accepted->stamp.token == binding.token
                   ? ReadResult::Unchanged
                   : ReadResult::NotReady;
    }
    return source->active_gamepad && source->active_gamepad->id == binding.resource_id &&
                   source->active_gamepad->token == binding.token && source->token == binding.token
               ? ReadResult::Unchanged
               : ReadResult::NotReady;
}

void VisualSceneRuntime::Revoke(const VisualAddress& address) noexcept {
    std::unique_lock lock(storage_->mutex);
    if (auto* source = Find(*storage_, address)) {
        source->revoked = true;
        source->publish_allowed = false;
        source->accepted = nullptr;
        source->accepted_pool = nullptr;
        source->active_pool = nullptr;
        source->active_gamepad = nullptr;
    }
}

void VisualSceneRuntime::CollectRetired() {
    std::unique_lock lock(storage_->mutex);
    Collect(*storage_, lock);
}

Outcome<std::monostate> StageGamepadData(VisualSceneRuntime& runtime, SourceToken token,
                                         std::shared_ptr<const GamepadSpriteSet> sprites,
                                         gamepad::HistoryReadView history) {
    if (!sprites)
        return Failure(EngineErrorCode::InvalidConfig, "gamepad data");
    auto validated = GamepadFrameSampler::Prepare(*sprites, history);
    if (!validated.succeeded())
        return *validated.error();
    auto& storage = *runtime.storage_;
    std::unique_lock lock(storage.mutex);
    auto* source = Find(storage, token.address);
    if (!source || source->revoked || source->token != token || !source->publish_allowed)
        return Failure(EngineErrorCode::StaleRevision, "gamepad request");
    if (!source->pools.empty())
        return Failure(EngineErrorCode::InvalidConfig, "source kind");
    if (source->image_preparing)
        return Failure(EngineErrorCode::Stalled, "image preparing");
    if (source->active_gamepad && source->active_gamepad->sprites == sprites &&
        source->active_gamepad->history == history) {
        // Scene bindings copy tokens, so held snapshots keep their old identity.
        source->active_gamepad->token = token;
        return std::monostate{};
    }
    Collect(storage, lock);
    source = Find(storage, token.address);
    if (!source || source->revoked || source->token != token || !source->publish_allowed)
        return Failure(EngineErrorCode::StaleRevision, "gamepad request");
    if (!source->pools.empty())
        return Failure(EngineErrorCode::InvalidConfig, "source kind");
    if (source->image_preparing)
        return Failure(EngineErrorCode::Stalled, "image preparing");
    if (!HasGenerationCapacity(storage, token.address.source))
        return Failure(EngineErrorCode::Stalled, "gamepad busy");
    uint64_t scratch, scratch_total, mapping, bytes;
    if (storage.next_resource_id == 0 || !Multiply(GamepadLayerQuadCapacity(*sprites), sizeof(SpriteQuad), scratch) ||
        !Multiply(scratch, storage.consumers.size(), scratch_total) ||
        !Multiply(sprites->parts.size(), sizeof(GamepadSpritePart), mapping) ||
        !Add(sprites->atlas->pixels.size(), mapping, bytes) || !Add(bytes, scratch_total, bytes) ||
        !Fits(storage, bytes))
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "gamepad storage");
    try {
        auto resource = std::make_shared<GamepadResource>();
        resource->bytes = bytes;
        resource->scratch_per_consumer = scratch;
        resource->token = token;
        resource->sprites = std::move(sprites);
        resource->history = std::move(history);
        source->gamepads.reserve(source->gamepads.size() + 1);
        resource->id = Next(storage.next_resource_id);
        source->active_gamepad = resource.get();
        source->gamepads.push_back(std::move(resource));
        storage.reserved_bytes += bytes;
        return std::monostate{};
    } catch (const std::bad_alloc&) {
        return Failure(EngineErrorCode::ResourceBudgetExceeded, "gamepad storage");
    }
}

} // namespace exosnap::engine
