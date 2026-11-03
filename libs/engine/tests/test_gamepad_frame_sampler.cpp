#include "gamepad_frame_sampler.h"
#include "visual_allocation_probe.h"

#include <exosnap/engine/gamepad_sprite_data.h>
#include <exosnap/engine/visual_scene_runtime.h>
#include <gamepad/gamepad.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>

using namespace exosnap;
using namespace exosnap::engine;

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

GamepadSpriteSet Sprites() {
    auto atlas = std::make_shared<SpriteAtlas>();
    atlas->width = 8;
    atlas->height = 8;
    atlas->generation = 1;
    atlas->pixels.resize(8 * 8 * 4);
    GamepadSpriteSet sprites;
    sprites.atlas = std::move(atlas);
    GamepadSpritePart body;
    body.idle = {0, 0, 4, 4};
    body.pressed = body.idle;
    body.bounds = {0, 0, 1, 1};
    sprites.parts.push_back(body);
    GamepadSpritePart stick;
    stick.idle = {4, 0, 2, 2};
    stick.pressed = {4, 2, 2, 2};
    stick.bounds = {0.1F, 0.2F, 0.1F, 0.1F};
    stick.motion = PadMotion::LeftStick;
    stick.click_control = gamepad::Control::LeftStickClick;
    stick.canonical = true;
    stick.knockout = {0, 4, 2, 2};
    stick.knockout_bounds = stick.bounds;
    sprites.parts.push_back(stick);
    return sprites;
}

gamepad::GamepadReading Reading(uint64_t time, float x, uint8_t device = 1) {
    gamepad::GamepadReading reading;
    reading.capture_100ns = time;
    reading.device.id.back() = device;
    reading.state.left_x = x;
    gamepad::NormalizeGameInputControls(reading.state, 0x3FFF);
    return reading;
}

} // namespace

TEST(GamepadHistoryView, MatchesExistingAtOrBeforeAndOutOfOrderSelection) {
    auto history = std::make_shared<gamepad::DeviceHistory>();
    const gamepad::HistoryReadView view(history);
    history->Push(Reading(200, 1));
    history->Push(Reading(100, 0.5F));
    gamepad::GamepadReading reading;
    EXPECT_EQ(view.TryStateAt(99, reading), gamepad::HistoryReadResult::Empty);
    for (uint64_t time : {100, 150, 200, 250}) {
        ASSERT_EQ(view.TryStateAt(time, reading), gamepad::HistoryReadResult::Found);
        const auto old = history->StateAt(time);
        ASSERT_TRUE(old);
        EXPECT_EQ(reading.capture_100ns, old->capture_100ns);
        EXPECT_EQ(reading.state, old->state);
        EXPECT_LE(reading.capture_100ns, time);
    }
    auto disconnected = Reading(300, 1);
    disconnected.connected = false;
    history->Push(disconnected);
    ASSERT_EQ(view.TryStateAt(350, reading), gamepad::HistoryReadResult::Found);
    EXPECT_FALSE(reading.connected);
    EXPECT_EQ(gamepad::ReadControl(reading.state, gamepad::Control::LeftStick).status,
              gamepad::ControlStatus::Unavailable);
}

TEST(GamepadHistoryView, RetainsOnlyHistoryAndEmptyViewIsSafe) {
    gamepad::HistoryReadView view;
    gamepad::GamepadReading reading;
    EXPECT_EQ(view.TryStateAt(100, reading), gamepad::HistoryReadResult::Empty);
    {
        auto history = std::make_shared<gamepad::DeviceHistory>();
        history->Push(Reading(100, 0.5F));
        view = gamepad::HistoryReadView(history);
    }
    ASSERT_EQ(view.TryStateAt(150, reading), gamepad::HistoryReadResult::Found);
    EXPECT_EQ(reading.capture_100ns, 100);
}

TEST(GamepadHistoryView, BothNestedLocksReturnBusyWithoutWaiting) {
    auto history = std::make_shared<gamepad::DeviceHistory>();
    const auto input = Reading(100, 1);
    history->Push(input);
    const gamepad::HistoryReadView view(history);
    for (auto* mutex : {&gamepad::HistoryReadTestAccess::Mutex(*history),
                        &gamepad::HistoryReadTestAccess::RingMutex(*history, input.device.id)}) {
        std::unique_lock lock(*mutex);
        auto result = std::async(std::launch::async, [&] {
            gamepad::GamepadReading reading;
            return view.TryStateAt(150, reading);
        });
        const auto status = result.wait_for(std::chrono::seconds(1));
        lock.unlock();
        EXPECT_EQ(status, std::future_status::ready);
        EXPECT_EQ(result.get(), gamepad::HistoryReadResult::Busy);
    }
}

TEST(GamepadFrameSampler, UsesPreparedScratchAndPreservesAtlasWhenSticksMove) {
    auto history = std::make_shared<gamepad::DeviceHistory>();
    auto sprites = Sprites();
    auto prepared = GamepadFrameSampler::Prepare(sprites, gamepad::HistoryReadView(history));
    ASSERT_TRUE(prepared.succeeded());
    auto& sampler = **prepared.value();
    SpriteLayerSnapshot output;
    ASSERT_TRUE(sampler.PrepareOutput(output).succeeded());
    const auto* atlas = output.atlas.get();
    const auto* pixels = output.atlas->pixels.data();
    const auto* quads = output.quads.data();
    const auto capacity = output.quads.capacity();
    const auto owners = output.atlas.use_count();
    SceneItemProperties properties;
    properties.placement = {0, 0, 1, 1};
    history->Push(Reading(100, 0));
    ASSERT_EQ(sampler.Sample(CaptureTime100ns{150}, properties, {0, 0, 480, 320}, output), ReadResult::Updated);
    ASSERT_EQ(output.quads.size(), 2);
    const auto resting = output.quads[1].destination;
    const auto mask = output.quads.front().masks[0].destination;
    history->Push(Reading(200, 1));
    visual_allocations::Probe allocations;
    ReadResult sample_result;
    {
        visual_allocations::Scope scope(allocations);
        sample_result = sampler.Sample(CaptureTime100ns{250}, properties, {0, 0, 480, 320}, output);
    }
    ASSERT_EQ(sample_result, ReadResult::Updated);
    EXPECT_EQ(allocations.allocations, 0);
    EXPECT_EQ(allocations.frees, 0);
    EXPECT_GT(output.quads[1].destination.x, resting.x);
    EXPECT_GT(output.quads.front().masks[0].destination.x, mask.x);
    EXPECT_EQ(output.quads.data(), quads);
    EXPECT_EQ(output.quads.capacity(), capacity);
    EXPECT_EQ(output.atlas.get(), atlas);
    EXPECT_EQ(output.atlas->pixels.data(), pixels);
    EXPECT_EQ(output.atlas->generation, 1);
    EXPECT_EQ(output.atlas.use_count(), owners);
}

TEST(GamepadFrameSampler, SelectionChangeAndBackwardsTimeNeverReusePressedFutureState) {
    auto history = std::make_shared<gamepad::DeviceHistory>();
    auto sprites = Sprites();
    auto prepared = GamepadFrameSampler::Prepare(sprites, gamepad::HistoryReadView(history));
    ASSERT_TRUE(prepared.succeeded());
    auto& sampler = **prepared.value();
    SpriteLayerSnapshot output;
    ASSERT_TRUE(sampler.PrepareOutput(output).succeeded());
    SceneItemProperties properties;
    properties.placement = {0, 0, 1, 1};
    history->Push(Reading(200, 1));
    ASSERT_EQ(sampler.Sample(CaptureTime100ns{250}, properties, {0, 0, 480, 320}, output), ReadResult::Updated);
    EXPECT_EQ(sampler.Sample(CaptureTime100ns{150}, properties, {0, 0, 480, 320}, output), ReadResult::NotReady);
    EXPECT_FLOAT_EQ(output.quads[1].destination.x, sprites.parts[1].bounds.x);
    ASSERT_EQ(sampler.Sample(CaptureTime100ns{250}, properties, {0, 0, 480, 320}, output), ReadResult::Updated);
    gamepad::DeviceId other{};
    other.back() = 2;
    history->Select(other);
    EXPECT_EQ(sampler.Sample(CaptureTime100ns{300}, properties, {0, 0, 480, 320}, output), ReadResult::NotReady);
    EXPECT_FLOAT_EQ(output.quads[1].destination.x, sprites.parts[1].bounds.x);
}

TEST(GamepadFrameSampler, TriggerScratchBoundCountsEveryTriggerPart) {
    auto sprites = Sprites();
    for (int index = 0; index < 4; ++index) {
        GamepadSpritePart trigger;
        trigger.idle = {0, 0, 4, 4};
        trigger.pressed = {4, 4, 4, 4};
        trigger.bounds = {0, 0, 0.25F, 0.25F};
        trigger.motion = PadMotion::LeftTrigger;
        sprites.parts.push_back(trigger);
    }
    auto history = std::make_shared<gamepad::DeviceHistory>();
    auto reading = Reading(100, 0);
    reading.state.controls[static_cast<size_t>(gamepad::Control::LeftTrigger)] = {gamepad::ControlStatus::Active, 1, 0};
    history->Push(reading);
    auto prepared = GamepadFrameSampler::Prepare(sprites, gamepad::HistoryReadView(history));
    ASSERT_TRUE(prepared.succeeded());
    SpriteLayerSnapshot output;
    ASSERT_TRUE((*prepared.value())->PrepareOutput(output).succeeded());
    const auto capacity = output.quads.capacity();
    ASSERT_GE(capacity, sprites.parts.size() + 4);
    EXPECT_EQ((*prepared.value())->Sample(CaptureTime100ns{150}, {}, {0, 0, 480, 320}, output), ReadResult::Updated);
    EXPECT_EQ(output.quads.size(), sprites.parts.size() + 4);
    EXPECT_EQ(output.quads.capacity(), capacity);
}

TEST(GamepadFrameSampler, BusyAfterSelectionChangeReplacesPressedFallbackWithNeutral) {
    auto history = std::make_shared<gamepad::DeviceHistory>();
    auto sprites = Sprites();
    auto prepared = GamepadFrameSampler::Prepare(sprites, gamepad::HistoryReadView(history));
    ASSERT_TRUE(prepared.succeeded());
    auto& sampler = **prepared.value();
    SpriteLayerSnapshot output;
    ASSERT_TRUE(sampler.PrepareOutput(output).succeeded());
    SceneItemProperties properties;
    properties.placement = {0, 0, 1, 1};
    history->Push(Reading(100, 1));
    ASSERT_EQ(sampler.Sample(CaptureTime100ns{150}, properties, {0, 0, 480, 320}, output), ReadResult::Updated);
    gamepad::DeviceId other{};
    other.back() = 2;
    history->Select(other);
    std::unique_lock lock(gamepad::HistoryReadTestAccess::Mutex(*history));
    auto attempt = std::async(std::launch::async, [&] {
        return sampler.Sample(CaptureTime100ns{200}, properties, {0, 0, 480, 320}, output);
    });
    const auto status = attempt.wait_for(std::chrono::seconds(1));
    lock.unlock();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(attempt.get(), ReadResult::Busy);
    EXPECT_FLOAT_EQ(output.quads[1].destination.x, sprites.parts[1].bounds.x);
}

TEST(GamepadPublication, ScenePinDefersPayloadDeleterUntilControlCollection) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    VisualAddress address;
    address.source.bytes.back() = 1;
    address.runtime.value = 1;
    const SourceToken token{address, SourceRequestRevision{1}};
    ASSERT_TRUE(runtime.CreateSource(address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    auto sprites = std::make_shared<GamepadSpriteSet>(Sprites());
    std::weak_ptr<const GamepadSpriteSet> lifetime = sprites;
    ASSERT_TRUE(StageGamepadData(runtime, token, sprites, {}).succeeded());
    ASSERT_TRUE(StageGamepadData(runtime, token, sprites, {}).succeeded());
    PreparedSceneDescription description;
    description.revision.value = 1;
    description.binding_sources.push_back(address);
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    SceneReadLease scene;
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), scene), ReadResult::Updated);
    ASSERT_TRUE(std::holds_alternative<GamepadBinding>(scene.Bindings()[0].content));
    EXPECT_EQ(std::get<GamepadBinding>(scene.Bindings()[0].content).sprites, sprites.get());
    sprites.reset();
    runtime.Revoke(address);
    description.binding_sources.clear();
    description.revision.value = 2;
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    runtime.CollectRetired();
    EXPECT_FALSE(lifetime.expired());
    auto release = std::async(std::launch::async, [held = std::move(scene)]() mutable { held.Reset(); });
    release.get();
    EXPECT_FALSE(lifetime.expired());
    runtime.CollectRetired();
    EXPECT_TRUE(lifetime.expired());
}

TEST(GamepadPublication, StalePreparationCannotReplaceNewRequest) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    VisualAddress address;
    address.source.bytes.back() = 1;
    address.runtime.value = 1;
    const SourceToken old_token{address, SourceRequestRevision{1}};
    const SourceToken current{address, SourceRequestRevision{2}};
    ASSERT_TRUE(runtime.CreateSource(address).succeeded());
    runtime.SetRequest(old_token, true, HoldPolicy::ClearAccepted);
    runtime.SetRequest(current, true, HoldPolicy::ClearAccepted);
    auto sprites = std::make_shared<GamepadSpriteSet>(Sprites());
    const auto stale = StageGamepadData(runtime, old_token, sprites, {});
    ASSERT_FALSE(stale.succeeded());
    EXPECT_EQ(stale.error()->code, EngineErrorCode::StaleRevision);
    EXPECT_TRUE(StageGamepadData(runtime, current, sprites, {}).succeeded());
}

TEST(GamepadPublication, SameAllocationKeepsResourceIdWithoutRelabellingHeldBinding) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    VisualAddress address;
    address.source.bytes.back() = 1;
    address.runtime.value = 1;
    const SourceToken old_token{address, SourceRequestRevision{1}};
    const SourceToken current{address, SourceRequestRevision{2}};
    ASSERT_TRUE(runtime.CreateSource(address).succeeded());
    runtime.SetRequest(old_token, true, HoldPolicy::ClearAccepted);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    auto sprites = std::make_shared<GamepadSpriteSet>(Sprites());
    ASSERT_TRUE(StageGamepadData(runtime, old_token, sprites, {}).succeeded());
    PreparedSceneDescription description;
    description.revision.value = 1;
    description.binding_sources.push_back(address);
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    SceneReadLease held;
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), held), ReadResult::Updated);
    const auto old_binding = held.Bindings()[0];
    runtime.SetRequest(current, true, HoldPolicy::KeepAccepted);
    ASSERT_TRUE(StageGamepadData(runtime, current, sprites, {}).succeeded());
    EXPECT_EQ(held.Bindings()[0].token, old_token);
    EXPECT_EQ(runtime.CheckBinding(old_binding), ReadResult::NotReady);
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    held.Reset();
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), held), ReadResult::Updated);
    EXPECT_EQ(held.Bindings()[0].token, current);
    EXPECT_EQ(held.Bindings()[0].resource_id, old_binding.resource_id);
    EXPECT_EQ(runtime.CheckBinding(held.Bindings()[0]), ReadResult::Unchanged);
}

TEST(GamepadPublication, ConsumerAtlasOwnerCannotBecomeFinalPayloadOwner) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    VisualAddress address;
    address.source.bytes.back() = 1;
    address.runtime.value = 1;
    const SourceToken token{address, SourceRequestRevision{1}};
    ASSERT_TRUE(runtime.CreateSource(address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto sprites = std::make_shared<GamepadSpriteSet>(Sprites());
    std::weak_ptr<const GamepadSpriteSet> lifetime = sprites;
    std::weak_ptr<const SpriteAtlas> atlas_lifetime = sprites->atlas;
    SpriteLayerSnapshot consumer_output;
    ASSERT_TRUE(PrepareGamepadLayer(*sprites, consumer_output).succeeded());
    ASSERT_TRUE(StageGamepadData(runtime, token, sprites, {}).succeeded());
    sprites.reset();
    runtime.Revoke(address);
    runtime.CollectRetired();
    EXPECT_FALSE(lifetime.expired());
    auto release = std::async(std::launch::async, [&] { consumer_output.atlas.reset(); });
    release.get();
    EXPECT_FALSE(atlas_lifetime.expired());
    runtime.CollectRetired();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_TRUE(atlas_lifetime.expired());
}

TEST(GamepadPublication, DistinctMappingsSharingAtlasDoNotBlockRetirement) {
    VisualSceneRuntime runtime{VisualBudgetPolicy{}};
    VisualAddress address;
    address.source.bytes.back() = 1;
    address.runtime.value = 1;
    SourceToken token{address, SourceRequestRevision{1}};
    ASSERT_TRUE(runtime.CreateSource(address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto first = std::make_shared<GamepadSpriteSet>(Sprites());
    auto second = std::make_shared<GamepadSpriteSet>(*first);
    auto third = std::make_shared<GamepadSpriteSet>(*first);
    std::weak_ptr<const GamepadSpriteSet> first_lifetime = first;
    SpriteLayerSnapshot consumer_output;
    ASSERT_TRUE(PrepareGamepadLayer(*first, consumer_output).succeeded());
    ASSERT_TRUE(StageGamepadData(runtime, token, first, {}).succeeded());
    first.reset();
    token.request.value = 2;
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    ASSERT_TRUE(StageGamepadData(runtime, token, second, {}).succeeded());
    second.reset();
    // Keep the third mapping from adding an unregistered atlas owner during collection.
    third->atlas.reset();
    runtime.CollectRetired();
    EXPECT_FALSE(first_lifetime.expired());
    consumer_output.atlas.reset();
    runtime.CollectRetired();
    EXPECT_TRUE(first_lifetime.expired());
    PreparedSceneDescription description;
    description.revision.value = 1;
    description.binding_sources.push_back(address);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    ASSERT_TRUE(runtime.PublishScene(description).succeeded());
    SceneReadLease scene;
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), scene), ReadResult::Updated);
    third->atlas = std::get<GamepadBinding>(scene.Bindings()[0].content).sprites->atlas;
    scene.Reset();
    token.request.value = 3;
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    EXPECT_TRUE(StageGamepadData(runtime, token, third, {}).succeeded());
}
