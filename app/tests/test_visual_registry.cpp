#include "extensions/CapabilityRegistry.h"
#include "models/VisualSceneConfig.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace exosnap {
namespace {

class FakeSource final : public VisualSource {
  public:
    explicit FakeSource(SourceCreateRequest request = {})
        : settings(std::move(request.validated_settings)), post_status(std::move(request.post_status)) {
    }
    engine::Outcome<std::monostate> Apply(const SourceRequest& request) override {
        settings = request.validated_settings;
        if (post_status)
            post_status({request.token, SourcePhase::Ready, {}});
        return std::monostate{};
    }
    void RequestStop() noexcept override {
    }
    bool IsQuiescent() const noexcept override {
        return true;
    }
    toml::table settings;
    std::function<void(SourceStatus)> post_status;
};

VisualSourceType FakeType(std::string id = "test/image", uint32_t limit = 2) {
    VisualSourceType type;
    type.id = std::move(id);
    type.label = {"VisualTests", "Image"};
    type.instance_limit = limit;
    type.settings.validate = [](const toml::table& settings) -> engine::Outcome<toml::table> { return settings; };
    type.create = [](SourceCreateRequest request) -> engine::Outcome<std::unique_ptr<VisualSource>> {
        return std::unique_ptr<VisualSource>(std::make_unique<FakeSource>(std::move(request)));
    };
    return type;
}

VisualSceneConfig Scene(std::string type = "test/image") {
    VisualSceneConfig scene;
    scene.sources.push_back({engine::VisualSourceId{{1}}, std::move(type), 1, true, {}});
    SceneItemRecord item;
    item.id = engine::SceneItemId{{2}};
    item.source = scene.sources.front().id;
    scene.items.push_back(item);
    return scene;
}

TEST(VisualRegistry, DuplicateTypeIsAtomicFailure) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {FakeType()}}).succeeded());
    EXPECT_FALSE(registry.Register({{"test/other"}, {FakeType("test/new"), FakeType()}}).succeeded());
    EXPECT_EQ(registry.VisualSources().size(), 1u);
    EXPECT_EQ(registry.FindVisualSource("test/new"), nullptr);
    EXPECT_TRUE(registry.Register({{"test/other"}, {FakeType("test/new")}}).succeeded());
}

TEST(VisualRegistry, DuplicateWithinModuleIsAtomicFailure) {
    CapabilityRegistry registry;
    EXPECT_FALSE(registry.Register({{"test/module"}, {FakeType(), FakeType()}}).succeeded());
    EXPECT_TRUE(registry.VisualSources().empty());
    EXPECT_TRUE(registry.Register({{"test/module"}, {FakeType()}}).succeeded());
}

TEST(VisualRegistry, SealRejectsMutation) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {FakeType()}}).succeeded());
    ASSERT_TRUE(registry.Seal().succeeded());
    EXPECT_TRUE(registry.Seal().succeeded());
    EXPECT_FALSE(registry.Register({{"test/new"}, {FakeType("test/new")}}).succeeded());
    EXPECT_EQ(registry.VisualSources().size(), 1u);
}

TEST(VisualRegistry, DiscoveryAndFactoryDoNotOpenHardware) {
    int created = 0;
    auto type = FakeType();
    type.create = [&created](SourceCreateRequest) -> engine::Outcome<std::unique_ptr<VisualSource>> {
        ++created;
        return std::unique_ptr<VisualSource>(std::make_unique<FakeSource>());
    };
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {std::move(type)}}).succeeded());
    ASSERT_TRUE(registry.Seal().succeeded());
    ASSERT_NE(registry.FindVisualSource("test/image"), nullptr);
    EXPECT_EQ(registry.VisualSources().size(), 1u);
    EXPECT_EQ(created, 0);
    auto instance = registry.FindVisualSource("test/image")->create({});
    ASSERT_TRUE(instance.succeeded());
    EXPECT_EQ(created, 1);
    EXPECT_TRUE((*instance.value())->IsQuiescent());
}

TEST(VisualRegistry, FakeInstancesAreIndependent) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {FakeType()}}).succeeded());
    const auto* type = registry.FindVisualSource("test/image");
    ASSERT_NE(type, nullptr);
    int first_status = 0;
    int second_status = 0;
    SourceCreateRequest first_request;
    first_request.validated_settings.insert("name", "first");
    first_request.post_status = [&first_status](SourceStatus) { ++first_status; };
    SourceCreateRequest second_request;
    second_request.validated_settings.insert("name", "second");
    second_request.post_status = [&second_status](SourceStatus) { ++second_status; };
    auto first = type->create(std::move(first_request));
    auto second = type->create(std::move(second_request));
    ASSERT_TRUE(first.succeeded());
    ASSERT_TRUE(second.succeeded());
    EXPECT_NE(first.value()->get(), second.value()->get());
    const auto* second_source = static_cast<const FakeSource*>(second.value()->get());
    SourceRequest edit;
    edit.validated_settings.insert("name", "changed");
    EXPECT_TRUE((*first.value())->Apply(edit).succeeded());
    EXPECT_EQ(second_source->settings["name"].value<std::string>(), "second");
    EXPECT_EQ(first_status, 1);
    EXPECT_EQ(second_status, 0);
}

TEST(VisualRegistry, InvalidAndIncompleteRegistrationsAreAtomic) {
    for (const auto& id :
         {std::string{}, std::string("Test/image"), std::string("test image"), std::string(129, 'a')}) {
        CapabilityRegistry registry;
        EXPECT_FALSE(registry.Register({{"test/module"}, {FakeType(id)}}).succeeded());
        EXPECT_TRUE(registry.VisualSources().empty());
    }
    CapabilityRegistry registry;
    auto type = FakeType();
    type.create = {};
    EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
    type = FakeType();
    type.settings.validate = {};
    EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
    type = FakeType();
    type.settings_version = 0;
    EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
    EXPECT_TRUE(registry.VisualSources().empty());
}

TEST(VisualRegistry, DuplicateModuleRejectedAndOrderStable) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {FakeType("test/z"), FakeType("test/a")}}).succeeded());
    EXPECT_FALSE(registry.Register({{"test/module"}, {FakeType("test/new")}}).succeeded());
    ASSERT_EQ(registry.VisualSources().size(), 2u);
    EXPECT_EQ(registry.VisualSources()[0].id, "test/z");
    EXPECT_EQ(registry.VisualSources()[1].id, "test/a");
}

TEST(VisualScene, PilotSecondInstanceRejected) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"org.exosnap.webcam"}, {FakeType("org.exosnap.webcam/camera", 1)}}).succeeded());
    auto scene = Scene("org.exosnap.webcam/camera");
    auto second = scene.sources.front();
    second.id = engine::VisualSourceId{{3}};
    scene.sources.push_back(second);
    EXPECT_FALSE(ValidateVisualScene(scene, registry).succeeded());
}

TEST(VisualScene, TwoItemsMayReferenceOneFakeSource) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {FakeType()}}).succeeded());
    auto scene = Scene();
    auto item = scene.items.front();
    item.id = engine::SceneItemId{{3}};
    scene.items.push_back(item);
    EXPECT_TRUE(ValidateVisualScene(scene, registry).succeeded());
}

TEST(VisualScene, InvalidIdentityReferenceAndGeometryAreRejectedWithoutMutation) {
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"test/module"}, {FakeType()}}).succeeded());
    auto scene = Scene();
    scene.sources.push_back(scene.sources.front());
    EXPECT_FALSE(ValidateVisualScene(scene, registry).succeeded());
    scene = Scene();
    scene.items.push_back(scene.items.front());
    EXPECT_FALSE(ValidateVisualScene(scene, registry).succeeded());
    scene = Scene();
    scene.sources.front().id = {};
    EXPECT_FALSE(ValidateVisualScene(scene, registry).succeeded());
    scene = Scene();
    scene.items.front().source = engine::VisualSourceId{{99}};
    EXPECT_FALSE(ValidateVisualScene(scene, registry).succeeded());
    scene = Scene();
    scene.items.front().properties.opacity = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(ValidateVisualScene(scene, registry).succeeded());
    EXPECT_TRUE(std::isnan(scene.items.front().properties.opacity));
}

TEST(VisualScene, UnknownTypesAndFutureSettingsRemainOpaque) {
    CapabilityRegistry registry;
    auto type = FakeType();
    type.settings.validate = [](const toml::table&) -> engine::Outcome<toml::table> { return engine::EngineError{}; };
    ASSERT_TRUE(registry.Register({{"test/module"}, {std::move(type)}}).succeeded());
    auto unknown = Scene("future/image");
    unknown.sources.front().settings.insert("future", "preserved");
    auto validated = ValidateVisualScene(unknown, registry);
    ASSERT_TRUE(validated.succeeded());
    EXPECT_EQ(validated.value()->sources.front().settings["future"].value<std::string>(), "preserved");
    auto future = Scene();
    future.sources.front().settings_version = 2;
    EXPECT_TRUE(ValidateVisualScene(future, registry).succeeded());
    future.sources.front().settings_version = 1;
    EXPECT_FALSE(ValidateVisualScene(future, registry).succeeded());
}

} // namespace
} // namespace exosnap
