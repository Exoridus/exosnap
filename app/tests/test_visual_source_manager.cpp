#include "extensions/VisualSourceManager.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace exosnap {
namespace {

struct FakeControl {
    std::vector<SourceRequest> requests;
    std::function<void(SourceStatus)> post;
    engine::VisualPublicationWriter publication;
    bool quiescent = true;
    bool stopped = false;
    bool reject = false;
    bool synchronous_ready = false;
    bool retain_accepted = false;
    int destroyed = 0;
    bool PublishReady() {
        const auto token = requests.back().token;
        if (!publication.PrepareImage(token, {1, 1}).succeeded())
            return false;
        auto frame = publication.TryBeginCpuFrame({token, requests.size(), {1}, {}});
        if (!frame)
            return false;
        std::fill(frame->Pixels().begin(), frame->Pixels().end(), 42);
        if (publication.Publish(std::move(*frame)) != engine::PublishResult::Accepted)
            return false;
        post({token, SourcePhase::Ready, {}});
        return true;
    }
};

class ManagedFake final : public VisualSource {
  public:
    explicit ManagedFake(std::shared_ptr<FakeControl> control) : control_(std::move(control)) {
    }
    ~ManagedFake() override {
        ++control_->destroyed;
    }
    engine::Outcome<std::monostate> Apply(const SourceRequest& request) override {
        control_->requests.push_back(request);
        if (control_->reject)
            return engine::EngineError{.context = "Fake open failure"};
        if (control_->synchronous_ready && !control_->PublishReady())
            return engine::EngineError{.context = "Fake frame publication failed"};
        return std::monostate{};
    }
    void RequestStop() noexcept override {
        control_->stopped = true;
    }
    bool RetainsAccepted(const toml::table&) const override {
        return control_->retain_accepted;
    }
    bool IsQuiescent() const noexcept override {
        return control_->quiescent;
    }

  private:
    std::shared_ptr<FakeControl> control_;
};

class VisualSourceManagerTest : public testing::Test {
  protected:
    CapabilityRegistry registry;
    engine::VisualSceneRuntime runtime{{}};
    std::vector<std::shared_ptr<FakeControl>> controls;
    bool factory_fails = false;
    void Register(bool seal = true) {
        VisualSourceType type;
        type.id = "test/image";
        type.label = {"Tests", "Image"};
        type.instance_limit = 2;
        type.settings.validate = [](const toml::table& table) -> engine::Outcome<toml::table> { return table; };
        type.create = [this](SourceCreateRequest request) -> engine::Outcome<std::unique_ptr<VisualSource>> {
            if (factory_fails)
                return engine::EngineError{.context = "Fake factory failure"};
            auto control = std::make_shared<FakeControl>();
            control->post = std::move(request.post_status);
            control->publication = std::move(request.publication);
            controls.push_back(control);
            return std::unique_ptr<VisualSource>(std::make_unique<ManagedFake>(std::move(control)));
        };
        ASSERT_TRUE(registry.Register({{"test/module"}, {std::move(type)}}).succeeded());
        if (seal)
            ASSERT_TRUE(registry.Seal().succeeded());
    }
    VisualSceneConfig Scene() {
        VisualSceneConfig scene;
        scene.sources.push_back({engine::VisualSourceId{{1}}, "test/image", 1, true, {}});
        SceneItemRecord item;
        item.id = engine::SceneItemId{{2}};
        item.source = scene.sources[0].id;
        scene.items.push_back(item);
        return scene;
    }
};

TEST_F(VisualSourceManagerTest, RequiresASealedRegistry) {
    Register(false);
    VisualSourceManager manager(registry, runtime);
    EXPECT_FALSE(manager.ApplyDesired(Scene(), {1}).succeeded());
    EXPECT_TRUE(controls.empty());
}

TEST_F(VisualSourceManagerTest, ValidatesWholeSceneBeforeTouchingActiveSources) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto scene = Scene();
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    const auto count = controls[0]->requests.size();
    scene.items[0].source = engine::VisualSourceId{{99}};
    EXPECT_FALSE(manager.ApplyDesired(scene, {2}).succeeded());
    EXPECT_EQ(controls[0]->requests.size(), count);
    EXPECT_FALSE(controls[0]->stopped);
}

TEST_F(VisualSourceManagerTest, StagesFactoriesBeforeApplyingAnyChanges) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto scene = Scene();
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    const auto count = controls[0]->requests.size();
    scene.sources[0].settings.insert("mode", 1);
    scene.sources.push_back({engine::VisualSourceId{{3}}, "test/image", 1, true, {}});
    factory_fails = true;
    EXPECT_FALSE(manager.ApplyDesired(scene, {2}).succeeded());
    EXPECT_EQ(controls[0]->requests.size(), count);
}

TEST_F(VisualSourceManagerTest, DemandBitsShareOneSourceAndOnlyZeroCrossingsChangeToken) {
    Register();
    VisualSourceManager manager(registry, runtime);
    ASSERT_TRUE(manager.ApplyDesired(Scene(), {1}).succeeded());
    auto control = controls[0];
    const auto dormant = control->requests.back().token;
    manager.SetConsumerDemand(VisualConsumer::SettingsPreview, true);
    const auto active = control->requests.back().token;
    EXPECT_GT(active.request.value, dormant.request.value);
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    EXPECT_EQ(control->requests.back().token, active);
    EXPECT_EQ(control->requests.back().demand.consumers, 6);
    manager.SetConsumerDemand(VisualConsumer::SettingsPreview, false);
    EXPECT_EQ(control->requests.back().demand.consumers, 4);
    EXPECT_EQ(control->requests.back().token, active);
    manager.SetConsumerDemand(VisualConsumer::Recording, false);
    EXPECT_GT(control->requests.back().token.request.value, active.request.value);
}

TEST_F(VisualSourceManagerTest, OpaqueVersionsStayInactiveAndDisabledSourcesReceiveZeroDemand) {
    Register();
    VisualSourceManager manager(registry, runtime);
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    auto scene = Scene();
    scene.sources[0].settings_version = 2;
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    EXPECT_TRUE(controls.empty());
    scene.sources[0].settings_version = 1;
    scene.sources[0].enabled = false;
    ASSERT_TRUE(manager.ApplyDesired(scene, {2}).succeeded());
    ASSERT_EQ(controls.size(), 1u);
    EXPECT_EQ(controls[0]->requests.back().demand.consumers, 0);
}

TEST_F(VisualSourceManagerTest, ItemEditsPreserveSourceTokenAndSameRevisionRejectsDifferentContent) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto scene = Scene();
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    const auto token = controls[0]->requests.back().token;
    scene.items[0].properties.opacity = 0.5f;
    EXPECT_FALSE(manager.ApplyDesired(scene, {1}).succeeded());
    ASSERT_TRUE(manager.ApplyDesired(scene, {2}).succeeded());
    EXPECT_EQ(controls[0]->requests.back().token, token);
}

TEST_F(VisualSourceManagerTest, TwoOrderedItemsShareOneSourceBinding) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    auto scene = Scene();
    scene.items[0].properties.order = 2;
    auto second = scene.items[0];
    second.id = engine::SceneItemId{{3}};
    second.properties.order = 1;
    scene.items.push_back(second);
    manager.SetConsumerDemand(VisualConsumer::RecordPreview, true);
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    EXPECT_EQ(controls.size(), 1u);
    ASSERT_TRUE(controls[0]->PublishReady());
    engine::SceneReadLease held;
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), held), engine::ReadResult::Updated);
    ASSERT_EQ(held.Items().size(), 2u);
    EXPECT_EQ(held.Bindings().size(), 1u);
    EXPECT_EQ(held.Items()[0].id, second.id);
    EXPECT_EQ(held.Items()[0].binding_index, held.Items()[1].binding_index);
    held.Reset();
    runtime.UnregisterConsumer(*consumer.value());
}

TEST_F(VisualSourceManagerTest, StaleStatusCannotChangeCurrentSourceAndOptionalFailureIsIsolated) {
    Register();
    VisualSourceManager manager(registry, runtime);
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    auto scene = Scene();
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    const auto old = controls[0]->requests.back().token;
    scene.sources[0].settings.insert("mode", 1);
    ASSERT_TRUE(manager.ApplyDesired(scene, {2}).succeeded());
    const auto current = controls[0]->requests.back().token;
    manager.OnSourceStatus({old, SourcePhase::Failed, engine::EngineError{.context = "stale"}});
    EXPECT_NE(manager.Status(current.address)->phase, SourcePhase::Failed);
    manager.OnSourceStatus({current, SourcePhase::Failed, engine::EngineError{.context = "open"}});
    EXPECT_EQ(manager.Status(current.address)->phase, SourcePhase::Failed);
    manager.OnSourceStatus({current, SourcePhase::Ready, {}});
    EXPECT_EQ(manager.Status(current.address)->phase, SourcePhase::Failed);
    EXPECT_FALSE(manager.Retry({current.address.source, {999}}).succeeded());
}

TEST_F(VisualSourceManagerTest, OneApplyFailureDoesNotRollbackOrDisableAnotherSource) {
    Register();
    VisualSourceManager manager(registry, runtime);
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    auto scene = Scene();
    scene.sources.push_back({engine::VisualSourceId{{3}}, "test/image", 1, true, {}});
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    ASSERT_EQ(controls.size(), 2u);
    controls[0]->reject = true;
    scene.sources[0].settings.insert("mode", 1);
    ASSERT_TRUE(manager.ApplyDesired(scene, {2}).succeeded());
    EXPECT_EQ(manager.Status(controls[0]->requests.back().token.address)->phase, SourcePhase::Failed);
    EXPECT_NE(manager.Status(controls[1]->requests.back().token.address)->phase, SourcePhase::Failed);
    EXPECT_EQ(controls[1]->requests.back().demand.consumers, 4);
    EXPECT_FALSE(controls[1]->stopped);
}

TEST_F(VisualSourceManagerTest, LiveSourceChangeRetainsOriginalStampedImageUntilReplacementArrives) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    auto scene = Scene();
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    ASSERT_TRUE(controls[0]->PublishReady());
    const auto old = controls[0]->requests.back().token;
    controls[0]->retain_accepted = true;
    scene.sources[0].settings.insert("brightness", 10);
    ASSERT_TRUE(manager.ApplyDesired(scene, {2}).succeeded());
    const auto current = controls[0]->requests.back().token;
    EXPECT_NE(current, old);
    engine::SceneReadLease prepared;
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), prepared), engine::ReadResult::Updated);
    ASSERT_EQ(prepared.Items().size(), 1u);
    engine::VisualReadLease image;
    ASSERT_EQ(runtime.TryReadImage(*consumer.value(), old.address, image), engine::ReadResult::Updated);
    EXPECT_EQ(image.Stamp().token, old);
    image.Reset();
    prepared.Reset();
    EXPECT_FALSE(controls[0]->publication.TryBeginCpuFrame({old, 99, {2}, {}}));
    ASSERT_TRUE(controls[0]->PublishReady());
    ASSERT_EQ(runtime.TryReadImage(*consumer.value(), current.address, image), engine::ReadResult::Updated);
    EXPECT_EQ(image.Stamp().token, current);
    image.Reset();
    runtime.UnregisterConsumer(*consumer.value());
}

TEST_F(VisualSourceManagerTest, NeverReadySourceDoesNotBlockAnotherAndDisconnectKeepsAcceptedContent) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    auto scene = Scene();
    scene.sources.push_back({engine::VisualSourceId{{3}}, "test/image", 1, true, {}});
    auto second = scene.items.front();
    second.id = engine::SceneItemId{{4}};
    second.source = scene.sources.back().id;
    scene.items.push_back(second);
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    ASSERT_TRUE(controls[0]->PublishReady());
    controls[1]->post({controls[1]->requests.back().token, SourcePhase::Waiting, {}});
    EXPECT_FALSE(manager.ProjectionError());
    engine::SceneReadLease held;
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), held), engine::ReadResult::Updated);
    EXPECT_EQ(held.Items().size(), 1u);
    held.Reset();
    ASSERT_TRUE(controls[1]->PublishReady());
    controls[0]->post({controls[0]->requests.back().token, SourcePhase::Waiting, {}});
    scene.items[0].properties.opacity = 0.5f;
    ASSERT_TRUE(manager.ApplyDesired(scene, {2}).succeeded());
    EXPECT_FALSE(manager.ProjectionError());
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), held), engine::ReadResult::Updated);
    EXPECT_EQ(held.Items().size(), 2u);
    held.Reset();
    scene.sources[0].settings.insert("mode", 1);
    ASSERT_TRUE(manager.ApplyDesired(scene, {3}).succeeded());
    ASSERT_EQ(runtime.TryReadScene(*consumer.value(), held), engine::ReadResult::Updated);
    EXPECT_EQ(held.Items().size(), 1u);
    held.Reset();
    runtime.UnregisterConsumer(*consumer.value());
}

TEST_F(VisualSourceManagerTest, RetainsStoppedProducerUntilQuiescentAndNeverReusesGeneration) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto scene = Scene();
    ASSERT_TRUE(manager.ApplyDesired(scene, {1}).succeeded());
    const auto old = controls[0]->requests.back().token;
    controls[0]->quiescent = false;
    ASSERT_TRUE(manager.ApplyDesired({}, {2}).succeeded());
    EXPECT_TRUE(controls[0]->stopped);
    EXPECT_EQ(controls[0]->destroyed, 0);
    EXPECT_FALSE(manager.IsQuiescent());
    EXPECT_FALSE(manager.ApplyDesired(scene, {3}).succeeded());
    controls[0]->quiescent = true;
    manager.CollectRetired();
    EXPECT_EQ(controls[0]->destroyed, 1);
    ASSERT_TRUE(manager.ApplyDesired(scene, {3}).succeeded());
    EXPECT_GT(controls.back()->requests.back().token.address.runtime.value, old.address.runtime.value);
    manager.RequestShutdown();
    EXPECT_TRUE(manager.IsQuiescent());
    EXPECT_FALSE(manager.ApplyDesired(scene, {4}).succeeded());
}

TEST_F(VisualSourceManagerTest, ShutdownRevokesNewReadsWhileHeldConsumerDataSurvives) {
    Register();
    VisualSourceManager manager(registry, runtime);
    auto consumer = runtime.RegisterConsumer();
    ASSERT_TRUE(consumer.succeeded());
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    ASSERT_TRUE(manager.ApplyDesired(Scene(), {1}).succeeded());
    auto control = controls[0];
    const auto token = control->requests.back().token;
    engine::VisualReadLease held;
    EXPECT_EQ(runtime.TryReadImage(*consumer.value(), token.address, held), engine::ReadResult::NotReady);
    ASSERT_TRUE(control->publication.PrepareImage(token, {1, 1}).succeeded());
    auto frame = control->publication.TryBeginCpuFrame({token, 1, {1}, {}});
    ASSERT_TRUE(frame);
    std::fill(frame->Pixels().begin(), frame->Pixels().end(), 42);
    ASSERT_EQ(control->publication.Publish(std::move(*frame)), engine::PublishResult::Accepted);
    ASSERT_EQ(runtime.TryReadImage(*consumer.value(), token.address, held), engine::ReadResult::Updated);
    manager.RequestShutdown();
    manager.CollectRetired();
    EXPECT_EQ(held.Image().pixels.front(), 42);
    held.Reset();
    EXPECT_EQ(runtime.TryReadImage(*consumer.value(), token.address, held), engine::ReadResult::Revoked);
    runtime.UnregisterConsumer(*consumer.value());
}

TEST_F(VisualSourceManagerTest, DeferredRetryWaitsForProducerAndThenUsesANewIncarnation) {
    Register();
    VisualSourceManager manager(registry, runtime);
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    ASSERT_TRUE(manager.ApplyDesired(Scene(), {1}).succeeded());
    auto old = controls[0];
    const auto token = old->requests.back().token;
    old->quiescent = false;
    ASSERT_TRUE(manager.Retry(token.address).succeeded());
    EXPECT_TRUE(old->stopped);
    EXPECT_EQ(controls.size(), 1u);
    manager.CollectRetired();
    EXPECT_EQ(controls.size(), 1u);
    old->quiescent = true;
    manager.CollectRetired();
    ASSERT_EQ(controls.size(), 2u);
    EXPECT_NE(controls[1]->requests.back().token.address, token.address);
    old->post({token, SourcePhase::Failed, engine::EngineError{.context = "late reconnect"}});
    EXPECT_NE(manager.Status(controls[1]->requests.back().token.address)->phase, SourcePhase::Failed);
}

TEST_F(VisualSourceManagerTest, SynchronousCompletionObservesCoherentStateAndAllowsObserverReentry) {
    Register();
    VisualSourceManager* current = nullptr;
    bool edited = false;
    VisualSourceManager manager(registry, runtime, [&](SourceStatus status) {
        EXPECT_EQ(current->Status(status.token.address)->phase, status.phase);
        if (!edited && status.phase == SourcePhase::Ready) {
            edited = true;
            auto scene = Scene();
            scene.items[0].properties.opacity = 0.5f;
            EXPECT_TRUE(current->ApplyDesired(scene, {2}).succeeded());
        }
    });
    current = &manager;
    manager.SetConsumerDemand(VisualConsumer::Recording, true);
    ASSERT_TRUE(manager.ApplyDesired(Scene(), {1}).succeeded());
    controls[0]->synchronous_ready = true;
    ASSERT_TRUE(manager.ApplyDesired(Scene(), {1}).succeeded());
    EXPECT_TRUE(edited);
    EXPECT_EQ(manager.Status(controls[0]->requests.back().token.address)->phase, SourcePhase::Ready);
}

} // namespace
} // namespace exosnap
