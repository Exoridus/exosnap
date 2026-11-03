#include "visual_allocation_probe.h"
#include "visual_scene_consumer.h"
#include "visual_scene_runtime_storage.h"

#include <exosnap/engine/gamepad_sprite_data.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <tuple>

using namespace exosnap;
using namespace exosnap::engine;

namespace exosnap::engine {
class VisualRuntimeTestAccess {
  public:
    static detail::VisualRuntimeStorage& Storage(VisualSceneRuntime& runtime) {
        return *runtime.storage_;
    }
};
} // namespace exosnap::engine

namespace exosnap::gamepad {
class HistoryReadTestAccess {
  public:
    static std::mutex& Mutex(DeviceHistory& history) {
        return history.mutex_;
    }
    static std::mutex& RingMutex(DeviceHistory& history, const DeviceId& device) {
        return history.devices_.at(device).history.mutex_;
    }
};
} // namespace exosnap::gamepad

namespace {

VisualAddress Source(uint8_t id, uint64_t generation) {
    VisualAddress address;
    address.source.bytes.back() = id;
    address.runtime.value = generation;
    return address;
}

class SceneConsumer : public testing::Test {
  protected:
    void SetUp() override {
        auto created = runtime->CreateSource(camera.address);
        ASSERT_TRUE(created.succeeded());
        writer = std::move(*created.value());
        runtime->SetRequest(camera, true, HoldPolicy::ClearAccepted);
        ASSERT_TRUE(writer.PrepareImage(camera, {2, 2}).succeeded());
        ASSERT_EQ(PublishImage(1), PublishResult::Accepted);
        ASSERT_TRUE(runtime->CreateSource(pad.address).succeeded());
        runtime->SetRequest(pad, true, HoldPolicy::ClearAccepted);
        auto atlas = std::make_shared<SpriteAtlas>();
        atlas->width = 8;
        atlas->height = 8;
        atlas->generation = 1;
        atlas->pixels.resize(8 * 8 * 4);
        auto sprites = std::make_shared<GamepadSpriteSet>();
        sprites->atlas = std::move(atlas);
        GamepadSpritePart body;
        body.idle = {0, 0, 4, 4};
        body.pressed = body.idle;
        body.bounds = {0, 0, 1, 1};
        sprites->parts.push_back(body);
        ASSERT_TRUE(StageGamepadData(*runtime, pad, std::move(sprites), {}).succeeded());
        description.revision.value = 1;
        description.binding_sources = {camera.address, pad.address};
        PreparedSceneItem image;
        image.id.bytes.back() = 1;
        image.source = camera.address;
        image.properties.placement = {0.1F, 0.2F, 0.25F, 0.25F};
        image.properties.opacity = 0.5F;
        description.items.push_back(image);
        PreparedSceneItem gamepad;
        gamepad.id.bytes.back() = 2;
        gamepad.source = pad.address;
        gamepad.binding_index = 1;
        gamepad.properties.placement = {0, 0, 1, 1};
        gamepad.properties.output_visible = false;
        gamepad.properties.opacity = 0.8F;
        description.items.push_back(gamepad);
        ASSERT_TRUE(runtime->PublishScene(description).succeeded());
        auto prepared = VisualSceneConsumer::Prepare(runtime);
        ASSERT_TRUE(prepared.succeeded());
        consumer = std::move(*prepared.value());
    }

    PublishResult PublishImage(uint64_t sequence) {
        auto frame = writer.TryBeginCpuFrame({camera, sequence, CaptureTime100ns{sequence * 100}, {}});
        if (!frame)
            return PublishResult::NoFreeSlot;
        std::ranges::fill(frame->Pixels(), static_cast<uint8_t>(sequence));
        return writer.Publish(std::move(*frame));
    }

    std::shared_ptr<VisualSceneRuntime> runtime = std::make_shared<VisualSceneRuntime>(VisualBudgetPolicy{});
    SourceToken camera{Source(1, 1), SourceRequestRevision{1}};
    SourceToken pad{Source(2, 2), SourceRequestRevision{1}};
    VisualPublicationWriter writer;
    PreparedSceneDescription description;
    std::unique_ptr<VisualSceneConsumer> consumer;
};

} // namespace

TEST_F(SceneConsumer, ModesPreserveOrderAndApplyOpacityExactlyOnce) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        EXPECT_EQ(frame.Items()[0].item->id.bytes.back(), 1);
        EXPECT_FLOAT_EQ(frame.Items()[0].image_opacity, 0.5F);
        EXPECT_NE(frame.Items()[0].image, nullptr);
    }
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::IdlePreview);
        ASSERT_EQ(frame.Items().size(), 2);
        EXPECT_EQ(frame.Items()[0].item->id.bytes.back(), 1);
        EXPECT_EQ(frame.Items()[1].item->id.bytes.back(), 2);
        ASSERT_NE(frame.Items()[1].sprites, nullptr);
        ASSERT_EQ(frame.Items()[1].sprites->quads.size(), 1);
        EXPECT_FLOAT_EQ(frame.Items()[1].sprites->quads[0].opacity, 0.8F * 0.35F);
    }
    {
        auto frame =
            consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::CompositedPreview);
        ASSERT_EQ(frame.Items().size(), 1);
        EXPECT_EQ(frame.Items()[0].item->id.bytes.back(), 2);
        EXPECT_FLOAT_EQ(frame.Items()[0].sprites->quads[0].opacity, 0.8F * 0.35F);
    }
}

TEST_F(SceneConsumer, HasItemsUsesThePreparedSceneAndCompositionMode) {
    EXPECT_TRUE(consumer->HasItems(VisualCompositionMode::Recording));
    EXPECT_TRUE(consumer->HasItems(VisualCompositionMode::IdlePreview));
    EXPECT_TRUE(consumer->HasItems(VisualCompositionMode::CompositedPreview));
    description.items[1].properties.output_visible = true;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    EXPECT_TRUE(consumer->HasItems(VisualCompositionMode::CompositedPreview));
    ASSERT_TRUE(consumer->Refresh().succeeded());
    EXPECT_FALSE(consumer->HasItems(VisualCompositionMode::CompositedPreview));
    for (auto& item : description.items)
        item.properties.output_visible = false;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    EXPECT_FALSE(consumer->HasItems(VisualCompositionMode::Recording));
    EXPECT_TRUE(consumer->HasItems(VisualCompositionMode::IdlePreview));
    consumer->Close();
    EXPECT_FALSE(consumer->HasItems(VisualCompositionMode::IdlePreview));
}

TEST_F(SceneConsumer, SteadySampleHasNoAllocationFreeOrFalseChange) {
    {
        auto first = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::IdlePreview);
        EXPECT_TRUE(first.Changed());
    }
    visual_allocations::Probe allocations;
    bool changed = true;
    size_t count = 0;
    bool cached = false;
    {
        visual_allocations::Scope scope(allocations);
        auto next = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::IdlePreview);
        changed = next.Changed();
        count = next.Items().size();
        cached = count != 0 && next.Items()[0].image == nullptr && next.Items()[0].cache_valid;
    }
    EXPECT_EQ(count, 2);
    EXPECT_TRUE(cached);
    EXPECT_FALSE(changed);
    EXPECT_EQ(allocations.allocations, 0);
    EXPECT_EQ(allocations.frees, 0);
}

TEST_F(SceneConsumer, PlacementRefreshPreservesUnchangedImageCacheIdentity) {
    uint64_t resource = 0;
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        resource = frame.Items()[0].resource_id;
    }
    description.items[0].properties.placement.x = 0.3F;
    description.revision.value = 2;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    EXPECT_TRUE(consumer->NeedsRefresh());
    const auto refreshed = consumer->Refresh();
    ASSERT_TRUE(refreshed.succeeded());
    EXPECT_EQ(*refreshed.value(), ReadResult::Updated);
    EXPECT_FALSE(consumer->NeedsRefresh());
    auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_TRUE(frame.Changed());
    EXPECT_EQ(frame.Items()[0].image, nullptr);
    EXPECT_TRUE(frame.Items()[0].cache_valid);
    EXPECT_EQ(frame.Items()[0].resource_id, resource);
    EXPECT_EQ(frame.Items()[0].image_rect.x, 144);
}

TEST_F(SceneConsumer, InvalidCameraDoesNotHideIndependentGamepad) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::IdlePreview);
        ASSERT_EQ(frame.Items().size(), 2);
    }
    ++camera.request.value;
    runtime->SetRequest(camera, true, HoldPolicy::ClearAccepted);
    auto frame = consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::IdlePreview);
    EXPECT_TRUE(frame.Changed());
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_EQ(frame.Items()[0].item->source, pad.address);
}

TEST_F(SceneConsumer, KeepAcceptedUsesOriginalStampUntilNewRequestPublishes) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
    }
    const auto old = camera;
    ++camera.request.value;
    runtime->SetRequest(camera, true, HoldPolicy::KeepAccepted);
    {
        auto frame = consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        EXPECT_EQ(frame.Items()[0].token, old);
        EXPECT_TRUE(frame.Items()[0].cache_valid);
    }
    ASSERT_EQ(PublishImage(1), PublishResult::Accepted);
    auto frame = consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_TRUE(frame.Changed());
    EXPECT_EQ(frame.Items()[0].token, camera);
    EXPECT_NE(frame.Items()[0].image, nullptr);
}

TEST_F(SceneConsumer, TwoItemsShareOneImageReadAndFrameRejectsConcurrentMaintenance) {
    auto duplicate = description.items[0];
    duplicate.id.bytes.back() = 3;
    description.items.push_back(duplicate);
    description.revision.value = 2;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 2);
    ASSERT_NE(frame.Items()[0].image, nullptr);
    EXPECT_EQ(frame.Items()[1].image, frame.Items()[0].image);
    EXPECT_EQ(frame.Items()[1].resource_id, frame.Items()[0].resource_id);
    const auto refreshed = consumer->Refresh();
    ASSERT_FALSE(refreshed.succeeded());
    EXPECT_EQ(refreshed.error()->code, EngineErrorCode::Stalled);
    runtime->Revoke(camera.address);
    runtime->CollectRetired();
    EXPECT_EQ(frame.Items()[0].image->pixels.front(), 1);
}

TEST_F(SceneConsumer, RecreatedConsumerCanUploadUnchangedPixelsAfterDeviceReset) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
    }
    consumer->Close();
    auto prepared = VisualSceneConsumer::Prepare(runtime);
    ASSERT_TRUE(prepared.succeeded());
    consumer = std::move(*prepared.value());
    auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_NE(frame.Items()[0].image, nullptr);
    EXPECT_EQ(frame.Items()[0].image_result, ReadResult::Updated);
}

TEST_F(SceneConsumer, BusyDefersCompositionWithoutAuthorizingCacheOrReportingRemoval) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
    }
    std::unique_lock lock(VisualRuntimeTestAccess::Storage(*runtime).mutex);
    auto attempt = std::async(std::launch::async, [&] {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        return std::tuple(frame.Items().size(), frame.Changed(), frame.Deferred());
    });
    const auto status = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    const auto [count, changed, deferred] = attempt.get();
    EXPECT_EQ(count, 0);
    EXPECT_FALSE(changed);
    EXPECT_TRUE(deferred);
    auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_FALSE(frame.Changed());
    EXPECT_FALSE(frame.Deferred());
    EXPECT_TRUE(frame.Items()[0].cache_valid);
    EXPECT_EQ(frame.Items()[0].image_result, ReadResult::Unchanged);
}

TEST_F(SceneConsumer, FrameReleaseAfterRetirementIsPinOnly) {
    visual_allocations::Probe allocations;
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        runtime->Revoke(camera.address);
        runtime->CollectRetired();
        visual_allocations::Scope scope(allocations);
        frame = {};
    }
    EXPECT_EQ(allocations.allocations, 0);
    EXPECT_EQ(allocations.frees, 0);
}

TEST_F(SceneConsumer, CpuPinsAreReleasedBeforeTheNextSample) {
    for (uint64_t sequence = 1; sequence < 12; ++sequence) {
        ASSERT_EQ(PublishImage(sequence + 1), PublishResult::Accepted);
        auto frame =
            consumer->Sample(CaptureTime100ns{sequence * 100}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        ASSERT_NE(frame.Items()[0].image, nullptr);
        EXPECT_EQ(frame.Items()[0].image_sequence, sequence + 1);
    }
}

TEST_F(SceneConsumer, ContentOffsetsAndDimensionsDirtySpritesWithoutNewInput) {
    const WebcamPixelRect rectangles[] = {{0, 0, 480, 320}, {10, 20, 480, 320}, {10, 20, 960, 640}};
    for (const auto& content : rectangles) {
        {
            auto frame = consumer->Sample(CaptureTime100ns{150}, content, VisualCompositionMode::CompositedPreview);
            ASSERT_EQ(frame.Items().size(), 1);
            EXPECT_TRUE(frame.Changed());
            EXPECT_EQ(frame.Items()[0].sample_result, ReadResult::NotReady);
        }
        auto steady = consumer->Sample(CaptureTime100ns{150}, content, VisualCompositionMode::CompositedPreview);
        EXPECT_FALSE(steady.Changed());
    }
}

TEST_F(SceneConsumer, SameConfigRevisionRefreshesNewResourceAndRemovedImageCanReturn) {
    uint64_t previous_resource = 0;
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        previous_resource = frame.Items()[0].resource_id;
    }
    ASSERT_TRUE(writer.PrepareImage(camera, {4, 4}).succeeded());
    ASSERT_EQ(PublishImage(2), PublishResult::Accepted);
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    EXPECT_TRUE(consumer->NeedsRefresh());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    {
        auto frame = consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        EXPECT_NE(frame.Items()[0].resource_id, previous_resource);
        ASSERT_NE(frame.Items()[0].image, nullptr);
        EXPECT_EQ(frame.Items()[0].image->width, 4);
    }
    const auto image = description.items.front();
    description.items.erase(description.items.begin());
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    description.items.insert(description.items.begin(), image);
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    auto frame = consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_TRUE(frame.Items()[0].cache_valid);
    EXPECT_EQ(frame.Items()[0].image_result, ReadResult::Updated);
    EXPECT_NE(frame.Items()[0].image, nullptr);
}

TEST_F(SceneConsumer, RetainsRuntimeUntilQuiescentClose) {
    std::weak_ptr<VisualSceneRuntime> retained = runtime;
    runtime.reset();
    EXPECT_FALSE(retained.expired());
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
        consumer->Close();
        EXPECT_FALSE(retained.expired());
        EXPECT_EQ(frame.Items()[0].image->pixels.front(), 1);
    }
    consumer->Close();
    EXPECT_TRUE(retained.expired());
}

TEST_F(SceneConsumer, NewRequestResetsGamepadFallbackEvenWithTheSameResource) {
    auto sprites = std::make_shared<GamepadSpriteSet>();
    auto atlas = std::make_shared<SpriteAtlas>();
    atlas->width = 8;
    atlas->height = 8;
    atlas->generation = 2;
    atlas->pixels.resize(8 * 8 * 4);
    sprites->atlas = std::move(atlas);
    GamepadSpritePart stick;
    stick.idle = {0, 0, 4, 4};
    stick.pressed = stick.idle;
    stick.bounds = {0.1F, 0.2F, 0.1F, 0.1F};
    stick.motion = PadMotion::LeftStick;
    sprites->parts.push_back(stick);
    auto history = std::make_shared<gamepad::DeviceHistory>();
    gamepad::GamepadReading reading;
    reading.capture_100ns = 100;
    reading.device.id.back() = 1;
    reading.state.left_x = 1;
    gamepad::NormalizeGameInputControls(reading.state, 0x3FFF);
    history->Push(reading);
    ASSERT_TRUE(StageGamepadData(*runtime, pad, sprites, gamepad::HistoryReadView(history)).succeeded());
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    uint64_t resource = 0;
    {
        auto frame =
            consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::CompositedPreview);
        ASSERT_EQ(frame.Items().size(), 1);
        resource = frame.Items()[0].resource_id;
        EXPECT_GT(frame.Items()[0].sprites->quads[0].destination.x, stick.bounds.x);
    }
    ++pad.request.value;
    runtime->SetRequest(pad, true, HoldPolicy::ClearAccepted);
    ASSERT_TRUE(StageGamepadData(*runtime, pad, sprites, gamepad::HistoryReadView(history)).succeeded());
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    std::unique_lock lock(gamepad::HistoryReadTestAccess::Mutex(*history));
    auto attempt = std::async(std::launch::async, [&] {
        auto frame =
            consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::CompositedPreview);
        EXPECT_EQ(frame.Items().size(), 1);
        if (frame.Items().empty())
            return;
        EXPECT_TRUE(frame.Changed());
        EXPECT_EQ(frame.Items()[0].token, pad);
        EXPECT_EQ(frame.Items()[0].resource_id, resource);
        EXPECT_EQ(frame.Items()[0].sample_result, ReadResult::Busy);
        EXPECT_FLOAT_EQ(frame.Items()[0].sprites->quads[0].destination.x, stick.bounds.x);
    });
    const auto status = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    attempt.get();
}

TEST_F(SceneConsumer, ImageSourcesArePreparedAndDeduplicatedOutsideSampling) {
    auto duplicate = description.items.front();
    duplicate.id.bytes.back() = 3;
    duplicate.binding_index = 2;
    description.items.push_back(duplicate);
    description.binding_sources.push_back(camera.address);
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    ASSERT_EQ(consumer->ImageSources().size(), 1);
    EXPECT_EQ(consumer->ImageSources()[0], camera.address);
    consumer->Close();
    EXPECT_TRUE(consumer->ImageSources().empty());
}

TEST_F(SceneConsumer, BusyRefreshRetainsOneOldScenePinAndDefersComposition) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1);
    }
    auto& storage = VisualRuntimeTestAccess::Storage(*runtime);
    auto* old_scene = storage.scene;
    EXPECT_EQ(old_scene->pins.load(), 1u);
    description.revision.value = 2;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    std::unique_lock lock(VisualRuntimeTestAccess::Storage(*runtime).mutex);
    auto attempt = std::async(std::launch::async, [&] { return consumer->Refresh(); });
    const auto status = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    const auto result = attempt.get();
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(*result.value(), ReadResult::Busy);
    EXPECT_EQ(old_scene->pins.load(), 1u);
    EXPECT_EQ(storage.scene->pins.load(), 0u);
    {
        auto frame = consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        EXPECT_TRUE(frame.Items().empty());
        EXPECT_FALSE(frame.Changed());
        EXPECT_TRUE(frame.Deferred());
        EXPECT_EQ(frame.Revision().value, 1u);
    }
    ASSERT_TRUE(consumer->Refresh().succeeded());
    EXPECT_EQ(old_scene->pins.load(), 0u);
    EXPECT_EQ(storage.scene->pins.load(), 1u);
    auto frame = consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    ASSERT_EQ(frame.Items().size(), 1);
    EXPECT_TRUE(frame.Changed());
    EXPECT_FALSE(frame.Deferred());
}

TEST_F(SceneConsumer, BusyBindingCheckAndFrameReleaseDoNotAllocateOrFree) {
    {
        auto frame =
            consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::CompositedPreview);
        ASSERT_EQ(frame.Items().size(), 1u);
    }
    std::unique_lock lock(VisualRuntimeTestAccess::Storage(*runtime).mutex);
    auto attempt = std::async(std::launch::async, [&] {
        visual_allocations::Probe probe;
        bool deferred;
        bool changed;
        size_t count;
        {
            visual_allocations::Scope scope(probe);
            auto frame =
                consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::CompositedPreview);
            deferred = frame.Deferred();
            changed = frame.Changed();
            count = frame.Items().size();
        }
        return std::tuple(deferred, changed, count, probe.allocations, probe.frees);
    });
    const auto status = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    const auto [deferred, changed, count, allocations, frees] = attempt.get();
    EXPECT_TRUE(deferred);
    EXPECT_FALSE(changed);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(frees, 0u);
    auto recovered =
        consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::CompositedPreview);
    EXPECT_FALSE(recovered.Deferred());
    EXPECT_FALSE(recovered.Changed());
    EXPECT_EQ(recovered.Items().size(), 1u);
}

TEST_F(SceneConsumer, UnadvertisedSceneKeepsTheOldPinUntilReplacementIsVisible) {
    auto& storage = VisualRuntimeTestAccess::Storage(*runtime);
    auto* first = storage.scene;
    description.revision.value = 2;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    const auto advertised = runtime->PublishedSceneSequence();
    description.revision.value = 3;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    const auto newest = runtime->PublishedSceneSequence();
    storage.scene_sequence.store(advertised, std::memory_order_release);
    const auto refreshed = consumer->Refresh();
    ASSERT_TRUE(refreshed.succeeded());
    EXPECT_EQ(*refreshed.value(), ReadResult::Busy);
    EXPECT_EQ(first->pins.load(), 1u);
    EXPECT_EQ(storage.scene->pins.load(), 0u);
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        EXPECT_TRUE(frame.Deferred());
        EXPECT_EQ(frame.Revision().value, 1u);
    }
    storage.scene_sequence.store(newest, std::memory_order_release);
    ASSERT_TRUE(consumer->Refresh().succeeded());
    EXPECT_EQ(first->pins.load(), 0u);
    EXPECT_EQ(storage.scene->pins.load(), 1u);
    auto frame = consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    EXPECT_FALSE(frame.Deferred());
    EXPECT_EQ(frame.Revision().value, 3u);
}

TEST_F(SceneConsumer, DeferredFrameRetriesTheImageWhoseUploadWasSkipped) {
    const SourceToken other{Source(3, 3), SourceRequestRevision{1}};
    auto created = runtime->CreateSource(other.address);
    ASSERT_TRUE(created.succeeded());
    runtime->SetRequest(other, true, HoldPolicy::ClearAccepted);
    ASSERT_TRUE(created.value()->PrepareImage(other, {2, 2}).succeeded());
    auto write = created.value()->TryBeginCpuFrame({other, 1, CaptureTime100ns{100}, {}});
    ASSERT_TRUE(write);
    ASSERT_EQ(created.value()->Publish(std::move(*write)), PublishResult::Accepted);
    auto item = description.items.front();
    item.id.bytes.back() = 3;
    item.source = other.address;
    item.binding_index = 2;
    description.items.push_back(item);
    description.binding_sources.push_back(other.address);
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(consumer->Refresh().succeeded());
    auto& storage = VisualRuntimeTestAccess::Storage(*runtime);
    ASSERT_EQ(storage.consumers.size(), 1u);
    VisualReadLease competing;
    ASSERT_EQ(runtime->TryReadImage(storage.consumers.begin()->first, other.address, competing), ReadResult::Updated);
    visual_allocations::Probe probe;
    bool deferred;
    size_t count;
    {
        visual_allocations::Scope scope(probe);
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        deferred = frame.Deferred();
        count = frame.Items().size();
    }
    EXPECT_TRUE(deferred);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(probe.allocations, 0u);
    EXPECT_EQ(probe.frees, 0u);
    competing.Reset();
    ASSERT_TRUE(runtime->InvalidateImage(storage.consumers.begin()->first, other.address).succeeded());
    EXPECT_TRUE(consumer->NeedsRefresh());
    {
        auto frame = consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        EXPECT_TRUE(frame.Deferred());
        EXPECT_TRUE(frame.Items().empty());
    }
    ASSERT_TRUE(consumer->Refresh().succeeded());
    auto recovered = consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    EXPECT_FALSE(recovered.Deferred());
    EXPECT_TRUE(recovered.Changed());
    ASSERT_EQ(recovered.Items().size(), 2u);
    EXPECT_EQ(recovered.Items().front().token, camera);
    EXPECT_EQ(recovered.Items().front().image_result, ReadResult::Updated);
    EXPECT_NE(recovered.Items().front().image, nullptr);
}

TEST_F(SceneConsumer, RevocationAfterDeferredReadNeverAuthorizesTheOldImage) {
    {
        auto frame = consumer->Sample(CaptureTime100ns{100}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        ASSERT_EQ(frame.Items().size(), 1u);
    }
    std::unique_lock lock(VisualRuntimeTestAccess::Storage(*runtime).mutex);
    auto attempt = std::async(std::launch::async, [&] {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        return frame.Deferred() && frame.Items().empty() && !frame.Changed();
    });
    const auto status = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(attempt.get());
    runtime->Revoke(camera.address);
    auto frame = consumer->Sample(CaptureTime100ns{200}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
    EXPECT_FALSE(frame.Deferred());
    EXPECT_TRUE(frame.Changed());
    EXPECT_TRUE(frame.Items().empty());
}

TEST_F(SceneConsumer, FrameReportsPinnedRevisionUntilMaintenanceAcceptsNewScene) {
    uint64_t firstPublication = 0;
    {
        auto frame = consumer->Sample(CaptureTime100ns{150}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        EXPECT_EQ(frame.Revision().value, 1U);
        firstPublication = frame.PublicationSequence();
        EXPECT_NE(firstPublication, 0U);
        description.revision.value = 2;
        ASSERT_TRUE(runtime->PublishScene(description).succeeded());
        EXPECT_EQ(frame.Revision().value, 1U);
        EXPECT_EQ(frame.PublicationSequence(), firstPublication);
    }
    ASSERT_TRUE(consumer->Refresh().succeeded());
    {
        auto frame = consumer->Sample(CaptureTime100ns{250}, {0, 0, 480, 320}, VisualCompositionMode::Recording);
        EXPECT_EQ(frame.Revision().value, 2U);
        EXPECT_GT(frame.PublicationSequence(), firstPublication);
        frame = {};
        EXPECT_EQ(frame.Revision().value, 0U);
        EXPECT_EQ(frame.PublicationSequence(), 0U);
    }
}
