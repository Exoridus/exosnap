#include "visual_allocation_probe.h"
#include "visual_scene_runtime_storage.h"
#include <exosnap/engine/visual_scene_runtime.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <limits>
#include <new>
#include <thread>
#include <tuple>
#include <utility>

using namespace exosnap::engine;

namespace visual_allocations {
thread_local Probe* active = nullptr;

void* Allocate(size_t size) {
    if (active) {
        ++active->allocations;
        if (active->fail) {
            active->fail = false;
            throw std::bad_alloc();
        }
        if (active->pause_size == size && active->entered && active->resume) {
            active->pause_size = 0;
            active->entered->release();
            active->resume->acquire();
        }
    }
    void* pointer = nullptr;
    while (!(pointer = std::malloc(size ? size : 1))) {
        const auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
    return pointer;
}

void Free(void* pointer) noexcept {
    if (active && pointer)
        ++active->frees;
    std::free(pointer);
}
} // namespace visual_allocations

void* operator new(size_t size) {
    return visual_allocations::Allocate(size);
}
void* operator new[](size_t size) {
    return visual_allocations::Allocate(size);
}
void operator delete(void* pointer) noexcept {
    visual_allocations::Free(pointer);
}
void operator delete[](void* pointer) noexcept {
    visual_allocations::Free(pointer);
}

namespace exosnap::engine {
class VisualRuntimeTestAccess {
  public:
    static detail::VisualRuntimeStorage& Storage(VisualSceneRuntime& runtime) {
        return *runtime.storage_;
    }
};
} // namespace exosnap::engine

namespace {

VisualAddress Address(uint8_t id = 1, uint64_t generation = 1) {
    VisualSourceId source;
    source.bytes.back() = id;
    return {source, VisualRuntimeGeneration{generation}};
}

class VisualRuntime : public testing::Test {
  protected:
    void SetUp() override {
        auto created = runtime.CreateSource(address);
        ASSERT_TRUE(created.succeeded());
        writer = std::move(*created.value());
        runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
        ASSERT_TRUE(writer.PrepareImage(token, {2, 2}).succeeded());
        auto registered = runtime.RegisterConsumer();
        ASSERT_TRUE(registered.succeeded());
        consumer = *registered.value();
    }

    PublishResult Publish(uint64_t sequence, uint8_t value = 11) {
        auto frame = writer.TryBeginCpuFrame({token, sequence, CaptureTime100ns{sequence * 100}, {}});
        if (!frame)
            return PublishResult::NoFreeSlot;
        std::ranges::fill(frame->Pixels(), value);
        return writer.Publish(std::move(*frame));
    }

    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    VisualAddress address = Address();
    SourceToken token{address, SourceRequestRevision{1}};
    VisualPublicationWriter writer;
    VisualConsumerId consumer = 0;
};

} // namespace

TEST_F(VisualRuntime, HeldFrameIsImmutable) {
    ASSERT_EQ(Publish(1, 11), PublishResult::Accepted);
    VisualReadLease held;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Updated);
    const auto* pixels = held.Image().pixels.data();
    const uint64_t resource = held.ResourceId();
    ASSERT_EQ(Publish(2, 22), PublishResult::Accepted);
    EXPECT_EQ(held.Image().pixels.data(), pixels);
    EXPECT_EQ(held.Image().pixels.front(), 11);
    EXPECT_EQ(held.Stamp().sequence, 1);
    EXPECT_EQ(held.ResourceId(), resource);
    held.Reset();
    ASSERT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Updated);
    EXPECT_EQ(held.Image().pixels.front(), 22);
    EXPECT_EQ(held.ResourceId(), resource);
}

TEST_F(VisualRuntime, AllSlotsPinnedDropsAndReleaseRestoresProducerProgress) {
    std::array<VisualReadLease, 3> held;
    std::array<VisualConsumerId, 3> readers{consumer, 0, 0};
    for (size_t index = 1; index < readers.size(); ++index) {
        auto registered = runtime.RegisterConsumer();
        ASSERT_TRUE(registered.succeeded());
        readers[index] = *registered.value();
    }
    for (size_t index = 0; index < held.size(); ++index) {
        ASSERT_EQ(Publish(index + 1, static_cast<uint8_t>(index)), PublishResult::Accepted);
        ASSERT_EQ(runtime.TryReadImage(readers[index], address, held[index]), ReadResult::Updated);
    }
    EXPECT_EQ(Publish(4), PublishResult::NoFreeSlot);
    for (size_t index = 0; index < held.size(); ++index)
        EXPECT_EQ(held[index].Image().pixels.front(), index);
    held[0].Reset();
    EXPECT_EQ(Publish(4), PublishResult::Accepted);
}

TEST_F(VisualRuntime, AbandonedWriterReturnsSlotAndMoveEmptiesSource) {
    auto first = writer.TryBeginCpuFrame({token, 1, CaptureTime100ns{100}, {}});
    ASSERT_TRUE(first);
    CpuFrameWriteLease moved = std::move(*first);
    EXPECT_FALSE(*first);
    ASSERT_TRUE(moved);
    moved.Reset();
    EXPECT_FALSE(moved);
    EXPECT_EQ(Publish(2), PublishResult::Accepted);
}

TEST(VisualRuntimeLifetime, ProducerLeaseRetainsStorageAfterWriterAndRuntimeRelease) {
    CpuFrameWriteLease held;
    {
        VisualSceneRuntime runtime{VisualBudgetPolicy{}};
        const SourceToken token{Address(), SourceRequestRevision{1}};
        auto created = runtime.CreateSource(token.address);
        ASSERT_TRUE(created.succeeded());
        runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
        ASSERT_TRUE(created.value()->PrepareImage(token, {2, 2}).succeeded());
        auto lease = created.value()->TryBeginCpuFrame({token, 1, CaptureTime100ns{100}, {}});
        ASSERT_TRUE(lease);
        held = std::move(*lease);
    }
    EXPECT_EQ(held.Pixels().size(), 16);
    std::ranges::fill(held.Pixels(), 42);
    EXPECT_EQ(held.Pixels().front(), 42);
    held.Reset();
}

TEST_F(VisualRuntime, OldRequestCannotPublishAfterDemandReopens) {
    auto old_frame = writer.TryBeginCpuFrame({token, 1, CaptureTime100ns{100}, {}});
    ASSERT_TRUE(old_frame);
    runtime.SetRequest({address, SourceRequestRevision{2}}, false, HoldPolicy::KeepAccepted);
    token.request.value = 3;
    runtime.SetRequest(token, true, HoldPolicy::KeepAccepted);
    EXPECT_EQ(writer.Publish(std::move(*old_frame)), PublishResult::Superseded);
    EXPECT_EQ(Publish(2), PublishResult::Accepted);
}

TEST_F(VisualRuntime, IdentitySwitchClearsAcceptedImageWithoutChangingHeldBytes) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    VisualReadLease held;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Updated);
    token.request.value = 2;
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto registered = runtime.RegisterConsumer();
    ASSERT_TRUE(registered.succeeded());
    VisualReadLease next;
    EXPECT_EQ(runtime.TryReadImage(*registered.value(), address, next), ReadResult::NotReady);
    EXPECT_FALSE(next);
    EXPECT_EQ(held.Image().pixels.front(), 11);
    held.Reset();
    EXPECT_EQ(Publish(2, 22), PublishResult::Accepted);
}

TEST_F(VisualRuntime, RevokeCannotBeUndoneByOldRequest) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    runtime.Revoke(address);
    runtime.SetRequest(token, true, HoldPolicy::KeepAccepted);
    EXPECT_FALSE(writer.TryBeginCpuFrame({token, 2, CaptureTime100ns{200}, {}}));
    VisualReadLease read;
    EXPECT_EQ(runtime.TryReadImage(consumer, address, read), ReadResult::Revoked);
    EXPECT_FALSE(read);
    EXPECT_FALSE(runtime.CreateSource(address).succeeded());
}

TEST_F(VisualRuntime, OlderSequenceCannotReplaceAcceptedFrame) {
    ASSERT_EQ(Publish(2, 22), PublishResult::Accepted);
    EXPECT_EQ(Publish(1, 11), PublishResult::Superseded);
    VisualReadLease read;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, read), ReadResult::Updated);
    EXPECT_EQ(read.Image().pixels.front(), 22);
}

TEST_F(VisualRuntime, UnchangedDoesNotReturnAnotherPin) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    VisualReadLease read;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, read), ReadResult::Updated);
    read.Reset();
    EXPECT_EQ(runtime.TryReadImage(consumer, address, read), ReadResult::Unchanged);
    EXPECT_FALSE(read);
}

TEST_F(VisualRuntime, BusyReadLeavesDestinationEmptyAndNeverWaitsForProducer) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    std::unique_lock lock(VisualRuntimeTestAccess::Storage(runtime).mutex);
    auto attempt = std::async(std::launch::async, [&] {
        VisualReadLease image;
        SceneReadLease scene;
        const auto image_result = runtime.TryReadImage(consumer, address, image);
        const auto scene_result = runtime.TryReadScene(consumer, scene);
        return std::tuple{image_result, scene_result, static_cast<bool>(image), static_cast<bool>(scene)};
    });
    const auto ready = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(ready, std::future_status::ready);
    const auto [image, scene, image_pin, scene_pin] = attempt.get();
    EXPECT_EQ(image, ReadResult::Busy);
    EXPECT_EQ(scene, ReadResult::Busy);
    EXPECT_FALSE(image_pin);
    EXPECT_FALSE(scene_pin);
}

TEST_F(VisualRuntime, UnregisterRequiresQuiescentReader) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    VisualReadLease held;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Updated);
    runtime.UnregisterConsumer(consumer);
    EXPECT_TRUE(VisualRuntimeTestAccess::Storage(runtime).consumers.contains(consumer));
    EXPECT_EQ(held.Image().pixels.front(), 11);
    held.Reset();
    runtime.UnregisterConsumer(consumer);
    EXPECT_FALSE(VisualRuntimeTestAccess::Storage(runtime).consumers.contains(consumer));
    EXPECT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Revoked);
    auto next = runtime.RegisterConsumer();
    ASSERT_TRUE(next.succeeded());
    EXPECT_GT(*next.value(), consumer);
}

TEST_F(VisualRuntime, NoPayloadDestructorOnConsumerThread) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    VisualReadLease held;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Updated);
    runtime.Revoke(address);
    runtime.CollectRetired();
    auto& storage = VisualRuntimeTestAccess::Storage(runtime);
    ASSERT_EQ(storage.sources.at(address)->pools.size(), 1);
    const auto bytes = storage.reserved_bytes;
    std::thread consumer_thread([lease = std::move(held)]() mutable { lease.Reset(); });
    consumer_thread.join();
    EXPECT_EQ(storage.sources.at(address)->pools.size(), 1);
    EXPECT_EQ(storage.reserved_bytes, bytes);
    runtime.CollectRetired();
    EXPECT_FALSE(storage.sources.contains(address));
    EXPECT_EQ(storage.reserved_bytes, 0);
}

TEST_F(VisualRuntime, RetiringLayoutBoundDefersThirdPoolUntilPinsDrain) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    VisualReadLease held;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, held), ReadResult::Updated);
    ASSERT_TRUE(writer.PrepareImage(token, {4, 2}).succeeded());
    ASSERT_EQ(Publish(2), PublishResult::Accepted);
    EXPECT_FALSE(writer.PrepareImage(token, {6, 2}).succeeded());
    EXPECT_EQ(held.Image().width, 2);
    held.Reset();
    runtime.CollectRetired();
    EXPECT_TRUE(writer.PrepareImage(token, {6, 2}).succeeded());
}

TEST_F(VisualRuntime, InvalidLayoutDoesNotReplaceAcceptedPool) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    EXPECT_FALSE(writer.PrepareImage(token, {3, 2, WebcamPixelFormat::Nv12}).succeeded());
    EXPECT_FALSE(writer.PrepareImage(token, {-1, 2}).succeeded());
    VisualReadLease read;
    ASSERT_EQ(runtime.TryReadImage(consumer, address, read), ReadResult::Updated);
    EXPECT_EQ(read.Image().width, 2);
}

TEST_F(VisualRuntime, PoolAllocationDoesNotHoldPublicationMutex) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    auto preparation = std::async(std::launch::async, [&] {
        visual_allocations::Probe probe;
        probe.pause_size = 16 * 16 * 4;
        probe.entered = &entered;
        probe.resume = &resume;
        visual_allocations::Scope scope(probe);
        return writer.PrepareImage(token, {16, 16});
    });
    const bool paused = entered.try_acquire_for(std::chrono::seconds(1));
    VisualReadLease read;
    const auto result = runtime.TryReadImage(consumer, address, read);
    resume.release();
    EXPECT_TRUE(paused);
    EXPECT_EQ(result, ReadResult::Updated);
    EXPECT_TRUE(preparation.get().succeeded());
}

TEST_F(VisualRuntime, PoolAllocationFailureRollsBackReservationAndKeepsAcceptedImage) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    const auto reserved = VisualRuntimeTestAccess::Storage(runtime).reserved_bytes;
    visual_allocations::Probe probe;
    probe.fail = true;
    Outcome<std::monostate> result{std::monostate{}};
    {
        visual_allocations::Scope scope(probe);
        result = writer.PrepareImage(token, {4, 4});
    }
    ASSERT_FALSE(result.succeeded());
    EXPECT_EQ(result.error()->code, EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_EQ(VisualRuntimeTestAccess::Storage(runtime).reserved_bytes, reserved);
    EXPECT_EQ(Publish(2), PublishResult::Accepted);
}

TEST_F(VisualRuntime, RequestChangeDuringPoolAllocationRejectsStagedPool) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    const auto reserved = VisualRuntimeTestAccess::Storage(runtime).reserved_bytes;
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    auto preparation = std::async(std::launch::async, [&] {
        visual_allocations::Probe probe;
        probe.pause_size = 16 * 16 * 4;
        probe.entered = &entered;
        probe.resume = &resume;
        visual_allocations::Scope scope(probe);
        return writer.PrepareImage(token, {16, 16});
    });
    const bool paused = entered.try_acquire_for(std::chrono::seconds(1));
    auto invalidate = std::async(std::launch::async, [&] {
        runtime.SetRequest({address, SourceRequestRevision{2}}, true, HoldPolicy::ClearAccepted);
    });
    const auto changed = invalidate.wait_for(std::chrono::seconds(1));
    resume.release();
    const auto result = preparation.get();
    invalidate.get();
    EXPECT_TRUE(paused);
    EXPECT_EQ(changed, std::future_status::ready);
    ASSERT_FALSE(result.succeeded());
    EXPECT_EQ(result.error()->code, EngineErrorCode::StaleRevision);
    EXPECT_EQ(VisualRuntimeTestAccess::Storage(runtime).reserved_bytes, reserved);
}

TEST_F(VisualRuntime, FramePublicationAndReaderReleaseDoNotAllocateOrFree) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    visual_allocations::Probe probe;
    ReadResult read_result;
    PublishResult published;
    {
        visual_allocations::Scope scope(probe);
        published = Publish(2);
        VisualReadLease read;
        read_result = runtime.TryReadImage(consumer, address, read);
        read.Reset();
    }
    EXPECT_EQ(published, PublishResult::Accepted);
    EXPECT_EQ(read_result, ReadResult::Updated);
    EXPECT_EQ(probe.allocations, 0);
    EXPECT_EQ(probe.frees, 0);
}

TEST(VisualRuntimeBudget, LargeValidLayoutHitsExplicitBudgetInsteadOfUnsupportedFormat) {
    VisualBudgetPolicy budget;
    budget.cpu_payload_limit_bytes = 256 * 1024 * 1024;
    VisualSceneRuntime runtime(budget);
    const SourceToken token{Address(), SourceRequestRevision{1}};
    auto created = runtime.CreateSource(token.address);
    ASSERT_TRUE(created.succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    const auto prepared = created.value()->PrepareImage(token, {7680, 4320});
    ASSERT_FALSE(prepared.succeeded());
    EXPECT_EQ(prepared.error()->code, EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_EQ(WebcamFramePayloadBytes(WebcamPixelFormat::Bgra, 7680, 4320) * 3, 398131200u);
    EXPECT_FALSE(VisualBudgetPolicy{}.cpu_payload_limit_bytes);
}

TEST(VisualRuntimeBudget, ReservationMultiplicationOverflowFailsBeforeAllocation) {
    VisualBudgetPolicy budget;
    budget.camera_slots = UINT32_MAX;
    VisualSceneRuntime runtime(budget);
    const SourceToken token{Address(), SourceRequestRevision{1}};
    auto created = runtime.CreateSource(token.address);
    ASSERT_TRUE(created.succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    const auto result = created.value()->PrepareImage(token, {INT32_MAX, INT32_MAX});
    ASSERT_FALSE(result.succeeded());
    EXPECT_EQ(result.error()->code, EngineErrorCode::ResourceBudgetExceeded);
    EXPECT_EQ(VisualRuntimeTestAccess::Storage(runtime).reserved_bytes, 0);
}

TEST_F(VisualRuntime, TwoItemsShareOneHostBindingAndReorderPreservesResource) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    PreparedSceneDescription description;
    description.revision.value = 1;
    description.binding_sources.push_back(address);
    for (uint8_t value : std::array<uint8_t, 2>{1, 2}) {
        PreparedSceneItem item;
        item.id.bytes.back() = value;
        item.source = address;
        description.items.push_back(item);
    }
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    SceneReadLease scene;
    ASSERT_EQ(runtime.TryReadScene(consumer, scene), ReadResult::Updated);
    ASSERT_EQ(scene.Items().size(), 2);
    ASSERT_EQ(scene.Bindings().size(), 1);
    const auto resource = scene.Bindings()[0].resource_id;
    EXPECT_NE(resource, 0);
    EXPECT_EQ(scene.Bindings()[0].token, token);
    scene.Reset();
    std::ranges::reverse(description.items);
    description.revision.value = 2;
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    ASSERT_EQ(runtime.TryReadScene(consumer, scene), ReadResult::Updated);
    EXPECT_EQ(scene.Items()[0].id.bytes.back(), 2);
    EXPECT_EQ(scene.Bindings()[0].resource_id, resource);
}

TEST_F(VisualRuntime, HeldSceneIsImmutableAndInvalidBindingFailsAtomically) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    PreparedSceneDescription description;
    description.revision.value = 1;
    description.binding_sources.push_back(address);
    PreparedSceneItem item;
    item.id.bytes.back() = 1;
    item.source = address;
    description.items.push_back(item);
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    SceneReadLease scene;
    ASSERT_EQ(runtime.TryReadScene(consumer, scene), ReadResult::Updated);
    description.revision.value = 2;
    description.items[0].binding_index = 10;
    EXPECT_FALSE(runtime.PublishScene(description).succeeded());
    EXPECT_EQ(scene.Revision().value, 1);
    EXPECT_EQ(scene.Items()[0].binding_index, 0);
    scene.Reset();
    EXPECT_EQ(runtime.TryReadScene(consumer, scene), ReadResult::Unchanged);
}

TEST_F(VisualRuntime, SameRevisionRebindAdvancesPublicationAndInvalidSourceSkipsIndependently) {
    ASSERT_EQ(Publish(1), PublishResult::Accepted);
    PreparedSceneDescription description;
    description.revision.value = 1;
    description.binding_sources.push_back(address);
    PreparedSceneItem item;
    item.id.bytes.back() = 1;
    item.source = address;
    description.items.push_back(item);
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    SceneReadLease scene;
    ASSERT_EQ(runtime.TryReadScene(consumer, scene), ReadResult::Updated);
    EXPECT_EQ(scene.PublicationSequence(), runtime.PublishedSceneSequence());
    const auto sequence = scene.PublicationSequence();
    EXPECT_EQ(runtime.CheckBinding(scene.Bindings()[0]), ReadResult::Unchanged);
    token.request.value = 2;
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    EXPECT_EQ(runtime.CheckBinding(scene.Bindings()[0]), ReadResult::NotReady);
    ASSERT_EQ(Publish(2), PublishResult::Accepted);
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    EXPECT_GT(runtime.PublishedSceneSequence(), sequence);
    EXPECT_EQ(scene.PublicationSequence(), sequence);
    scene.Reset();
    ASSERT_EQ(runtime.TryReadScene(consumer, scene), ReadResult::Updated);
    EXPECT_EQ(scene.Revision().value, 1);
    EXPECT_EQ(scene.Bindings()[0].token, token);
    EXPECT_EQ(runtime.CheckBinding(scene.Bindings()[0]), ReadResult::Unchanged);
}

TEST(VisualRuntimeLifetime, RevokedGenerationsReclaimSourceAndReaderMetadataWithoutReusingIds) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    for (uint64_t generation = 1; generation <= 32; ++generation) {
        const auto address = Address(1, generation);
        const SourceToken token{address, SourceRequestRevision{1}};
        auto created = runtime.CreateSource(address);
        ASSERT_TRUE(created.succeeded());
        runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
        ASSERT_TRUE(created.value()->PrepareImage(token, {2, 2}).succeeded());
        auto write = created.value()->TryBeginCpuFrame({token, 1, CaptureTime100ns{100}, {}});
        ASSERT_TRUE(write);
        ASSERT_EQ(created.value()->Publish(std::move(*write)), PublishResult::Accepted);
        VisualReadLease held;
        ASSERT_EQ(runtime.TryReadImage(*consumer.value(), address, held), ReadResult::Updated);
        runtime.Revoke(address);
        runtime.CollectRetired();
        auto& storage = VisualRuntimeTestAccess::Storage(runtime);
        EXPECT_EQ(storage.sources.size(), 1);
        held.Reset();
        runtime.CollectRetired();
        ASSERT_TRUE(storage.sources.empty());
        ASSERT_TRUE(storage.consumers.at(*consumer.value())->images.empty());
        EXPECT_FALSE(created.value()->TryBeginCpuFrame({token, 2, CaptureTime100ns{200}, {}}));
        EXPECT_FALSE(runtime.CreateSource(address).succeeded());
        ASSERT_FALSE(runtime.CreateSource(Address(2, generation)).succeeded());
    }
}

TEST(VisualRuntimeLifetime, FailedSourceAllocationDoesNotConsumeRuntimeGeneration) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    visual_allocations::Probe probe;
    probe.fail = true;
    {
        visual_allocations::Scope scope(probe);
        const auto failed = runtime.CreateSource(Address());
        EXPECT_FALSE(failed.succeeded());
    }
    EXPECT_TRUE(runtime.CreateSource(Address()).succeeded());
}
