#include <exosnap/engine/visual_frame.h>
#include <exosnap/engine/visual_gpu_lane.h>
#include <exosnap/engine/visual_scene.h>
#include <exosnap/engine/visual_scene_runtime.h>

#include "visual_allocation_probe.h"
#include "visual_scene_consumer.h"
#include "visual_scene_runtime_storage.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <winrt/base.h>

using namespace exosnap::engine;

namespace exosnap::engine {
class VisualRuntimeTestAccess {
  public:
    static detail::VisualRuntimeStorage& Storage(VisualSceneRuntime& runtime) {
        return *runtime.storage_;
    }
};
} // namespace exosnap::engine

namespace {

PreparedSceneDescription GpuScene(uint64_t revision = 1) {
    PreparedSceneDescription scene;
    scene.revision = ConfigRevision{revision};
    VisualSourceId source;
    source.bytes[0] = 1;
    scene.binding_sources.push_back({source, VisualRuntimeGeneration{1}});
    PreparedSceneItem item;
    item.id.bytes[0] = 1;
    item.source = scene.binding_sources.front();
    item.properties.placement = {0, 0, 1, 1};
    scene.items.push_back(item);
    return scene;
}

struct TestDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

TestDevice Warp() {
    TestDevice result;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, result.device.put(),
                                      nullptr, result.context.put());
    EXPECT_EQ(hr, S_OK);
    return result;
}

VisualGpuIdentity Identity(uint64_t resource = 7) {
    VisualSourceId source;
    source.bytes[0] = 1;
    return {{VisualAddress{source, VisualRuntimeGeneration{1}}, SourceRequestRevision{1}},
            resource,
            DeviceGeneration{1},
            DeviceGeneration{2}};
}

VisualFrameStamp Stamp(uint64_t sequence) {
    return {Identity().token, sequence, CaptureTime100ns{sequence * 100}, CaptureTime100ns{sequence * 90}};
}

winrt::com_ptr<ID3D11Texture2D> Texture(ID3D11Device* device, uint32_t pixel,
                                        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    const std::vector<uint32_t> pixels(16 * 8, pixel);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 16;
    description.Height = 8;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = 16 * 4;
    winrt::com_ptr<ID3D11Texture2D> result;
    EXPECT_EQ(device->CreateTexture2D(&description, &initial, result.put()), S_OK);
    return result;
}

// Settling and readback are evidence operations, never bridge frame operations.
template <typename Attempt> bool Eventually(Attempt attempt, const TestDevice& producer, const TestDevice& consumer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (attempt())
            return true;
        producer.context->Flush();
        consumer.context->Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

std::vector<uint32_t> ReadPixels(const TestDevice& consumer, ID3D11ShaderResourceView* view) {
    winrt::com_ptr<ID3D11Resource> resource;
    view->GetResource(resource.put());
    auto texture = resource.as<ID3D11Texture2D>();
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    EXPECT_EQ(consumer.device->CreateTexture2D(&description, nullptr, staging.put()), S_OK);
    if (!staging)
        return {};
    consumer.context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr = consumer.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    EXPECT_EQ(hr, S_OK);
    if (FAILED(hr))
        return {};
    std::vector<uint32_t> result(static_cast<size_t>(description.Width) * description.Height);
    for (UINT row = 0; row < description.Height; ++row)
        std::memcpy(result.data() + row * description.Width,
                    static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch, description.Width * 4);
    consumer.context->Unmap(staging.get(), 0);
    return result;
}

TEST(VisualGpuLane, RejectsWrongSourceDeviceFormatAndIdentity) {
    auto producer = Warp();
    auto consumer = Warp();
    ASSERT_TRUE(producer.device);
    ASSERT_TRUE(consumer.device);
    auto source = Texture(producer.device.get(), 0);
    auto wrong_device = Texture(consumer.device.get(), 0);
    auto wrong_format = Texture(producer.device.get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    ASSERT_TRUE(source);
    ASSERT_TRUE(wrong_device);
    ASSERT_TRUE(wrong_format);
    std::string error;
    EXPECT_FALSE(
        VisualGpuLane::Create(Identity(), wrong_device.get(), producer.context.get(), consumer.context.get(), error));
    EXPECT_FALSE(
        VisualGpuLane::Create(Identity(), wrong_format.get(), producer.context.get(), consumer.context.get(), error));
    EXPECT_FALSE(
        VisualGpuLane::Create(Identity(), source.get(), producer.context.get(), producer.context.get(), error));
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    description.MipLevels = 2;
    winrt::com_ptr<ID3D11Texture2D> wrong_shape;
    ASSERT_EQ(producer.device->CreateTexture2D(&description, nullptr, wrong_shape.put()), S_OK);
    EXPECT_FALSE(
        VisualGpuLane::Create(Identity(), wrong_shape.get(), producer.context.get(), consumer.context.get(), error));
    auto identity = Identity();
    identity.consumer = {};
    EXPECT_FALSE(VisualGpuLane::Create(identity, source.get(), producer.context.get(), consumer.context.get(), error));
    identity = Identity();
    identity.consumer = identity.producer;
    EXPECT_FALSE(VisualGpuLane::Create(identity, source.get(), producer.context.get(), consumer.context.get(), error));
    identity = Identity();
    identity.token.address.source = {};
    EXPECT_FALSE(VisualGpuLane::Create(identity, source.get(), producer.context.get(), consumer.context.get(), error));
    identity = Identity(0);
    EXPECT_FALSE(VisualGpuLane::Create(identity, source.get(), producer.context.get(), consumer.context.get(), error));
    winrt::com_ptr<ID3D11DeviceContext> deferred;
    ASSERT_EQ(producer.device->CreateDeferredContext(0, deferred.put()), S_OK);
    EXPECT_FALSE(VisualGpuLane::Create(Identity(), source.get(), deferred.get(), consumer.context.get(), error));
}

TEST(VisualGpuLane, PublishesExactStampRetainsLocalAndRejectsStaleSequence) {
    auto producer = Warp();
    auto consumer = Warp();
    ASSERT_TRUE(producer.device);
    ASSERT_TRUE(consumer.device);
    auto source = Texture(producer.device.get(), 0x80402010);
    ASSERT_TRUE(source);
    std::string error;
    auto lane = VisualGpuLane::Create(Identity(), source.get(), producer.context.get(), consumer.context.get(), error);
    ASSERT_TRUE(lane) << error;
    EXPECT_EQ(lane->LogicalTransportBytes(), 16u * 8u * 8u);
    VisualGpuFrame frame;
    EXPECT_EQ(lane->TryRead(frame), ReadResult::NotReady);
    EXPECT_EQ(frame.view, nullptr);
    EXPECT_EQ(lane->TryPublish(Stamp(0)), PublishResult::Invalid);
    auto wrong = Stamp(1);
    wrong.token.request.value = 2;
    EXPECT_EQ(lane->TryPublish(wrong), PublishResult::Invalid);
    ASSERT_TRUE(Eventually([&] { return lane->TryPublish(Stamp(1)) == PublishResult::Accepted; }, producer, consumer));
    EXPECT_EQ(lane->TryPublish(Stamp(2)), PublishResult::NoFreeSlot);
    ASSERT_TRUE(Eventually([&] { return lane->TryRead(frame) == ReadResult::Updated; }, producer, consumer));
    EXPECT_EQ(frame.stamp.sequence, 1u);
    EXPECT_EQ(frame.stamp.token, Identity().token);
    EXPECT_EQ(frame.stamp.received_at, Stamp(1).received_at);
    EXPECT_EQ(frame.stamp.captured_at, Stamp(1).captured_at);
    EXPECT_EQ(frame.identity.resource_id, 7u);
    EXPECT_EQ(frame.identity.producer, DeviceGeneration{1});
    EXPECT_EQ(frame.identity.consumer, DeviceGeneration{2});
    const auto* first_view = frame.view;
    EXPECT_EQ(ReadPixels(consumer, frame.view), std::vector<uint32_t>(16 * 8, 0x80402010));
    const std::vector<uint32_t> changed(16 * 8, 0xA0804020);
    producer.context->UpdateSubresource(source.get(), 0, nullptr, changed.data(), 16 * 4, 0);
    ASSERT_TRUE(Eventually([&] { return lane->TryPublish(Stamp(3)) == PublishResult::Accepted; }, producer, consumer));
    EXPECT_EQ(ReadPixels(consumer, frame.view), std::vector<uint32_t>(16 * 8, 0x80402010));
    EXPECT_EQ(lane->TryPublish(Stamp(2)), PublishResult::Superseded);
    ASSERT_TRUE(Eventually([&] { return lane->TryRead(frame) == ReadResult::Updated; }, producer, consumer));
    EXPECT_EQ(frame.stamp.sequence, 3u);
    EXPECT_EQ(frame.view, first_view);
    EXPECT_EQ(ReadPixels(consumer, frame.view), changed);
    EXPECT_EQ(lane->TryRead(frame), ReadResult::Unchanged);
    EXPECT_EQ(frame.stamp.sequence, 3u);
    lane->Retire();
    EXPECT_TRUE(lane->Retired());
    EXPECT_EQ(lane->TryPublish(Stamp(4)), PublishResult::Revoked);
    EXPECT_EQ(lane->TryRead(frame), ReadResult::Revoked);
    EXPECT_EQ(frame.view, nullptr);
}

TEST(VisualGpuLane, AnUnconsumedLaneDoesNotPreventAnotherConsumerFromProgressing) {
    auto producer = Warp();
    auto first_consumer = Warp();
    auto second_consumer = Warp();
    ASSERT_TRUE(producer.device);
    ASSERT_TRUE(first_consumer.device);
    ASSERT_TRUE(second_consumer.device);
    auto source = Texture(producer.device.get(), 0x80402010);
    ASSERT_TRUE(source);
    std::string error;
    auto first =
        VisualGpuLane::Create(Identity(), source.get(), producer.context.get(), first_consumer.context.get(), error);
    auto second_identity = Identity();
    second_identity.consumer = DeviceGeneration{3};
    auto second = VisualGpuLane::Create(second_identity, source.get(), producer.context.get(),
                                        second_consumer.context.get(), error);
    ASSERT_TRUE(first) << error;
    ASSERT_TRUE(second) << error;
    ASSERT_TRUE(
        Eventually([&] { return first->TryPublish(Stamp(1)) == PublishResult::Accepted; }, producer, first_consumer));
    ASSERT_TRUE(
        Eventually([&] { return second->TryPublish(Stamp(1)) == PublishResult::Accepted; }, producer, second_consumer));
    VisualGpuFrame frame;
    ASSERT_TRUE(Eventually([&] { return second->TryRead(frame) == ReadResult::Updated; }, producer, second_consumer));
    EXPECT_EQ(first->TryPublish(Stamp(2)), PublishResult::NoFreeSlot);
    ASSERT_TRUE(
        Eventually([&] { return second->TryPublish(Stamp(2)) == PublishResult::Accepted; }, producer, second_consumer));
    ASSERT_TRUE(Eventually([&] { return second->TryRead(frame) == ReadResult::Updated; }, producer, second_consumer));
    EXPECT_EQ(frame.stamp.sequence, 2u);
    EXPECT_EQ(frame.identity.consumer, DeviceGeneration{3});
    ASSERT_TRUE(Eventually([&] { return first->TryRead(frame) == ReadResult::Updated; }, producer, first_consumer));
    EXPECT_EQ(frame.stamp.sequence, 1u);
    EXPECT_EQ(frame.identity.consumer, DeviceGeneration{2});
}

TEST(VisualGpuRuntime, ReservationsBoundStagedGenerationsBeforeNativeAllocation) {
    auto consumer = Warp();
    VisualSceneRuntime runtime({});
    const auto token = Identity().token;
    ASSERT_TRUE(runtime.CreateSource(token.address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto registered = runtime.RegisterConsumer({consumer.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(registered.succeeded());
    auto first = ReserveGpuData(runtime, token, 16, 8);
    auto second = ReserveGpuData(runtime, token, 16, 8);
    ASSERT_TRUE(first.succeeded());
    ASSERT_TRUE(second.succeeded());
    auto dxgi = consumer.device.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    ASSERT_EQ(dxgi->GetAdapter(adapter.put()), S_OK);
    DXGI_ADAPTER_DESC description{};
    ASSERT_EQ(adapter->GetDesc(&description), S_OK);
    LUID copied{};
    ASSERT_TRUE(first.value()->AdapterLuid(copied));
    EXPECT_EQ(copied.LowPart, description.AdapterLuid.LowPart);
    EXPECT_EQ(copied.HighPart, description.AdapterLuid.HighPart);
    EXPECT_EQ(first.value()->TryPublish(Stamp(1)), PublishResult::Invalid);
    EXPECT_FALSE(ReserveGpuData(runtime, token, 16, 8).succeeded());
    first.value()->Reset();
    runtime.CollectRetired();
    auto third = ReserveGpuData(runtime, token, 16, 8);
    ASSERT_TRUE(third.succeeded());
    EXPECT_GT(third.value()->ResourceId(), second.value()->ResourceId());
    runtime.UnregisterConsumer(*registered.value());
    EXPECT_TRUE(second.value()->Retired());
    EXPECT_TRUE(third.value()->Retired());
    EXPECT_FALSE(second.value()->AdapterLuid(copied));
    second.value()->Reset();
    third.value()->Reset();
    runtime.CollectRetired();
}

TEST(VisualGpuRuntime, PinsExactBindingAndRetiresOnRequestChange) {
    auto producer = Warp();
    auto consumer = Warp();
    auto texture = Texture(producer.device.get(), 0x80402010);
    VisualSceneRuntime runtime({});
    const auto token = Identity().token;
    ASSERT_TRUE(runtime.CreateSource(token.address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto registered = runtime.RegisterConsumer({consumer.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(registered.succeeded());
    const auto id = *registered.value();
    auto staged = StageGpuData(runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(staged.succeeded());
    auto lease = std::move(*staged.value());
    ASSERT_TRUE(runtime.PublishScene(GpuScene()).succeeded());
    SceneReadLease scene;
    ASSERT_EQ(runtime.TryReadScene(id, scene), ReadResult::Updated);
    ASSERT_EQ(scene.Bindings().size(), 1u);
    const auto binding = scene.Bindings().front();
    ASSERT_TRUE(std::holds_alternative<GpuImageBinding>(binding.content));
    ASSERT_TRUE(Eventually([&] { return lease.TryPublish(Stamp(1)) == PublishResult::Accepted; }, producer, consumer));
    GpuReadLease frame;
    ASSERT_TRUE(
        Eventually([&] { return runtime.TryReadGpu(id, binding, frame) == ReadResult::Updated; }, producer, consumer));
    EXPECT_EQ(frame.Frame().identity.resource_id, lease.ResourceId());
    EXPECT_EQ(frame.Frame().identity.token, token);
    GpuReadLease other;
    EXPECT_EQ(runtime.TryReadGpu(id, binding, other), ReadResult::Busy);
    EXPECT_FALSE(other);
    visual_allocations::Probe probe;
    ReadResult unchanged;
    {
        visual_allocations::Scope scope(probe);
        frame.Reset();
        unchanged = runtime.TryReadGpu(id, binding, frame);
        frame.Reset();
    }
    EXPECT_EQ(unchanged, ReadResult::Unchanged);
    EXPECT_EQ(probe.allocations, 0u);
    EXPECT_EQ(probe.frees, 0u);
    auto next = token;
    ++next.request.value;
    runtime.SetRequest(next, true, HoldPolicy::KeepAccepted);
    EXPECT_TRUE(lease.Retired());
    EXPECT_EQ(lease.TryPublish(Stamp(2)), PublishResult::Revoked);
    EXPECT_EQ(runtime.TryReadGpu(id, binding, frame), ReadResult::NotReady);
    EXPECT_FALSE(frame);
    lease.Reset();
    scene.Reset();
    ASSERT_TRUE(runtime.PublishScene({ConfigRevision{2}, {}, {}}).succeeded());
    runtime.CollectRetired();
    runtime.UnregisterConsumer(id);
}

TEST(VisualGpuRuntime, TopologyReplacementIsBoundedAndDrainsHeldScenes) {
    auto producer = Warp();
    auto first = Warp();
    auto second = Warp();
    auto texture = Texture(producer.device.get(), 0);
    VisualSceneRuntime runtime({});
    const auto token = Identity().token;
    ASSERT_TRUE(runtime.CreateSource(token.address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto a = runtime.RegisterConsumer({first.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(a.succeeded());
    auto generation1 = StageGpuData(runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(generation1.succeeded());
    ASSERT_TRUE(runtime.PublishScene(GpuScene()).succeeded());
    SceneReadLease old_scene;
    ASSERT_EQ(runtime.TryReadScene(*a.value(), old_scene), ReadResult::Updated);
    auto b = runtime.RegisterConsumer({second.context.get(), DeviceGeneration{3}});
    ASSERT_TRUE(b.succeeded());
    EXPECT_TRUE(generation1.value()->Retired());
    generation1.value()->Reset();
    ASSERT_TRUE(runtime.PublishScene({ConfigRevision{2}, {}, {}}).succeeded());
    auto generation2 = StageGpuData(runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(generation2.succeeded());
    EXPECT_GT(generation2.value()->ResourceId(), old_scene.Bindings().front().resource_id);
    ASSERT_TRUE(runtime.PublishScene(GpuScene(3)).succeeded());
    auto observer = runtime.RegisterConsumer();
    ASSERT_TRUE(observer.succeeded());
    SceneReadLease newer_scene;
    ASSERT_EQ(runtime.TryReadScene(*observer.value(), newer_scene), ReadResult::Updated);
    runtime.UnregisterConsumer(*b.value());
    EXPECT_TRUE(generation2.value()->Retired());
    generation2.value()->Reset();
    ASSERT_TRUE(runtime.PublishScene({ConfigRevision{4}, {}, {}}).succeeded());
    auto blocked = StageGpuData(runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    EXPECT_FALSE(blocked.succeeded());
    old_scene.Reset();
    newer_scene.Reset();
    runtime.CollectRetired();
    auto generation3 = StageGpuData(runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(generation3.succeeded());
    generation3.value()->Reset();
    runtime.UnregisterConsumer(*a.value());
    runtime.UnregisterConsumer(*observer.value());
    runtime.CollectRetired();
}

TEST(VisualGpuRuntime, ConsumerSharesOnePinnedGpuFrameAcrossItems) {
    auto producer = Warp();
    auto consumer = Warp();
    auto texture = Texture(producer.device.get(), 0x80402010);
    auto runtime = std::make_shared<VisualSceneRuntime>(VisualBudgetPolicy{});
    const auto token = Identity().token;
    ASSERT_TRUE(runtime->CreateSource(token.address).succeeded());
    runtime->SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto prepared = VisualSceneConsumer::Prepare(runtime, {consumer.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(prepared.succeeded());
    auto& reader = **prepared.value();
    auto staged = StageGpuData(*runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(staged.succeeded());
    auto description = GpuScene();
    description.items.push_back(description.items.front());
    description.items.back().id.bytes[0] = 2;
    description.items.back().properties.opacity = 0.5F;
    description.items.back().properties.output_visible = false;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(reader.Refresh().succeeded());
    ASSERT_TRUE(Eventually([&] { return staged.value()->TryPublish(Stamp(1)) == PublishResult::Accepted; }, producer,
                           consumer));
    ASSERT_TRUE(Eventually(
        [&] {
            auto frame = reader.Sample(CaptureTime100ns{100}, {0, 0, 640, 480}, VisualCompositionMode::IdlePreview);
            if (frame.Items().size() != 2)
                return false;
            EXPECT_EQ(frame.Items()[0].gpu, frame.Items()[1].gpu);
            EXPECT_NE(frame.Items()[0].gpu, nullptr);
            const SourceToken required[] = {token, token};
            EXPECT_TRUE(frame.HasGpuFrames(required));
            auto changed = token;
            ++changed.request.value;
            const SourceToken old_request[] = {changed};
            EXPECT_FALSE(frame.HasGpuFrames(old_request));
            EXPECT_FLOAT_EQ(frame.Items()[1].image_opacity, 0.5F * 0.35F);
            return true;
        },
        producer, consumer));
    {
        auto frame = reader.Sample(CaptureTime100ns{100}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
        EXPECT_EQ(frame.Items().size(), 1u);
    }
    {
        auto frame = reader.Sample(CaptureTime100ns{100}, {0, 0, 640, 480}, VisualCompositionMode::CompositedPreview);
        EXPECT_EQ(frame.Items().size(), 1u);
    }
    staged.value()->Reset();
    reader.Close();
    ASSERT_TRUE(runtime->PublishScene({ConfigRevision{2}, {}, {}}).succeeded());
    runtime->CollectRetired();
}

TEST(VisualGpuRuntime, DeferredConsumerPreservesGpuChangeAndRetriesSkippedCpuUpload) {
    auto producer = Warp();
    auto device = Warp();
    auto texture = Texture(producer.device.get(), 0x80402010);
    auto runtime = std::make_shared<VisualSceneRuntime>(VisualBudgetPolicy{});
    const auto token = Identity().token;
    ASSERT_TRUE(runtime->CreateSource(token.address).succeeded());
    runtime->SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto cpu = token;
    cpu.address.source.bytes[0] = 2;
    cpu.address.runtime.value = 2;
    auto writer = runtime->CreateSource(cpu.address);
    ASSERT_TRUE(writer.succeeded());
    runtime->SetRequest(cpu, true, HoldPolicy::ClearAccepted);
    ASSERT_TRUE(writer.value()->PrepareImage(cpu, {2, 2}).succeeded());
    auto write = writer.value()->TryBeginCpuFrame({cpu, 1, CaptureTime100ns{100}, {}});
    ASSERT_TRUE(write);
    ASSERT_EQ(writer.value()->Publish(std::move(*write)), PublishResult::Accepted);
    auto prepared = VisualSceneConsumer::Prepare(runtime, {device.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(prepared.succeeded());
    auto& reader = **prepared.value();
    auto staged = StageGpuData(*runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(staged.succeeded());
    auto description = GpuScene();
    description.binding_sources.push_back(cpu.address);
    auto item = description.items.front();
    item.id.bytes[0] = 2;
    item.source = cpu.address;
    item.binding_index = 1;
    description.items.push_back(item);
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(reader.Refresh().succeeded());
    ASSERT_TRUE(
        Eventually([&] { return staged.value()->TryPublish(Stamp(1)) == PublishResult::Accepted; }, producer, device));
    ASSERT_TRUE(Eventually(
        [&] {
            auto frame = reader.Sample(CaptureTime100ns{100}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
            return !frame.Deferred() && frame.Items().size() == 2;
        },
        producer, device));
    {
        auto frame = reader.Sample(CaptureTime100ns{150}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
        ASSERT_FALSE(frame.Deferred());
        ASSERT_FALSE(frame.Changed());
    }

    auto& storage = VisualRuntimeTestAccess::Storage(*runtime);
    ASSERT_EQ(storage.consumers.size(), 1u);
    const auto consumer_id = storage.consumers.begin()->first;
    auto* gpu = storage.sources.at(token.address)->active_gpu;
    ASSERT_NE(gpu, nullptr);
    ASSERT_EQ(gpu->pins.load(), 1u);
    ASSERT_TRUE(runtime->InvalidateImage(consumer_id, cpu.address).succeeded());
    VisualReadLease competing_cpu;
    ASSERT_EQ(runtime->TryReadImage(consumer_id, cpu.address, competing_cpu), ReadResult::Updated);
    ASSERT_TRUE(
        Eventually([&] { return staged.value()->TryPublish(Stamp(2)) == PublishResult::Accepted; }, producer, device));
    ASSERT_TRUE(Eventually(
        [&] {
            auto frame = reader.Sample(CaptureTime100ns{200}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
            EXPECT_TRUE(frame.Deferred());
            EXPECT_FALSE(frame.Changed());
            EXPECT_TRUE(frame.Items().empty());
            // The second pin proves sampling consumed the new GPU frame before the CPU rejection.
            return gpu->pins.load(std::memory_order_acquire) == 2;
        },
        producer, device));
    competing_cpu.Reset();
    EXPECT_FALSE(reader.NeedsRefresh());
    {
        auto recovered = reader.Sample(CaptureTime100ns{250}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
        EXPECT_FALSE(recovered.Deferred());
        EXPECT_TRUE(recovered.Changed());
        ASSERT_EQ(recovered.Items().size(), 2u);
        ASSERT_NE(recovered.Items()[0].gpu, nullptr);
        EXPECT_EQ(recovered.Items()[0].gpu->stamp.sequence, 2u);
    }
    {
        auto unchanged = reader.Sample(CaptureTime100ns{300}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
        EXPECT_FALSE(unchanged.Deferred());
        EXPECT_FALSE(unchanged.Changed());
    }

    write = writer.value()->TryBeginCpuFrame({cpu, 2, CaptureTime100ns{350}, {}});
    ASSERT_TRUE(write);
    ASSERT_EQ(writer.value()->Publish(std::move(*write)), PublishResult::Accepted);
    std::swap(description.items[0], description.items[1]);
    description.revision.value = 2;
    ASSERT_TRUE(runtime->PublishScene(description).succeeded());
    ASSERT_TRUE(reader.Refresh().succeeded());
    GpuReadLease competing_gpu;
    ASSERT_EQ(runtime->TryReadGpu(consumer_id, storage.scene->bindings[0], competing_gpu), ReadResult::Unchanged);
    visual_allocations::Probe probe;
    bool deferred;
    bool changed;
    size_t count;
    {
        visual_allocations::Scope scope(probe);
        auto frame = reader.Sample(CaptureTime100ns{400}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
        deferred = frame.Deferred();
        changed = frame.Changed();
        count = frame.Items().size();
    }
    EXPECT_TRUE(deferred);
    EXPECT_FALSE(changed);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(probe.allocations, 0u);
    EXPECT_EQ(probe.frees, 0u);
    EXPECT_TRUE(reader.NeedsRefresh());
    competing_gpu.Reset();
    ASSERT_TRUE(reader.Refresh().succeeded());
    {
        auto recovered = reader.Sample(CaptureTime100ns{450}, {0, 0, 640, 480}, VisualCompositionMode::Recording);
        EXPECT_FALSE(recovered.Deferred());
        EXPECT_TRUE(recovered.Changed());
        ASSERT_EQ(recovered.Items().size(), 2u);
        EXPECT_EQ(recovered.Items()[0].token, cpu);
        EXPECT_EQ(recovered.Items()[0].image_result, ReadResult::Updated);
        EXPECT_NE(recovered.Items()[0].image, nullptr);
        EXPECT_EQ(recovered.Items()[0].image_sequence, 2u);
        EXPECT_NE(recovered.Items()[1].gpu, nullptr);
    }
    staged.value()->Reset();
    reader.Close();
    ASSERT_TRUE(runtime->PublishScene({ConfigRevision{3}, {}, {}}).succeeded());
    runtime->CollectRetired();
}

TEST(VisualGpuRuntime, BudgetRejectsBeforeAdmissionAndFailedReplacementPreservesBinding) {
    auto producer = Warp();
    auto consumer = Warp();
    auto texture = Texture(producer.device.get(), 0);
    VisualBudgetPolicy policy;
    policy.gpu_payload_limit_bytes = 16 * 8 * 4 * 3 - 1;
    VisualSceneRuntime limited(policy);
    const auto token = Identity().token;
    ASSERT_TRUE(limited.CreateSource(token.address).succeeded());
    limited.SetRequest(token, true, HoldPolicy::ClearAccepted);
    auto registered = limited.RegisterConsumer({consumer.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(registered.succeeded());
    EXPECT_FALSE(StageGpuData(limited, token, texture.get(), producer.context.get(), DeviceGeneration{1}).succeeded());
    limited.UnregisterConsumer(*registered.value());

    VisualSceneRuntime runtime({});
    ASSERT_TRUE(runtime.CreateSource(token.address).succeeded());
    runtime.SetRequest(token, true, HoldPolicy::ClearAccepted);
    registered = runtime.RegisterConsumer({consumer.context.get(), DeviceGeneration{2}});
    ASSERT_TRUE(registered.succeeded());
    auto good = StageGpuData(runtime, token, texture.get(), producer.context.get(), DeviceGeneration{1});
    ASSERT_TRUE(good.succeeded());
    auto wrong = Texture(producer.device.get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_FALSE(StageGpuData(runtime, token, wrong.get(), producer.context.get(), DeviceGeneration{1}).succeeded());
    EXPECT_FALSE(good.value()->Retired());
    good.value()->Reset();
    runtime.UnregisterConsumer(*registered.value());
    runtime.CollectRetired();
}

} // namespace
