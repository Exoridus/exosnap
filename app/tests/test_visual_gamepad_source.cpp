#include "extensions/GamepadModule.h"
#include "models/VisualSceneConfig.h"

#include <gtest/gtest.h>

namespace exosnap {
namespace {

struct GamepadHarness {
    int prepares = 0;
    int stops = 0;
    bool quiescent = true;
    engine::SourceToken last_token;
    GamepadOverlaySettings settings;
    std::function<void(SourceStatus)> completion;
    std::vector<SourceStatus> statuses;

    GamepadControlHooks Hooks() {
        return {[this](engine::SourceToken token, GamepadOverlaySettings value,
                       std::function<void(SourceStatus)> callback) {
                    ++prepares;
                    last_token = token;
                    settings = std::move(value);
                    completion = std::move(callback);
                    quiescent = false;
                },
                [this] { ++stops; }, [this] { return quiescent; }};
    }
    SourceCreateRequest Creation() {
        SourceCreateRequest result;
        result.token = {{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{1}},
                        engine::SourceRequestRevision{1}};
        result.validated_settings = toml::table{{"device", ""}, {"layout", "auto"}};
        result.post_status = [this](SourceStatus status) { statuses.push_back(std::move(status)); };
        return result;
    }
    SourceRequest Request(uint64_t revision = 1, uint8_t consumers = 1) {
        const auto creation = Creation();
        return {{creation.token.address, engine::SourceRequestRevision{revision}},
                engine::ConfigRevision{revision},
                {consumers},
                creation.validated_settings};
    }
};

TEST(VisualGamepadSource, FactoryIsInertAndFirstDemandPreparesUsingExactToken) {
    GamepadHarness harness;
    CapabilityRegistry registry;
    const auto options = [](const VisualSourceRecord&) { return ChoiceSnapshot{}; };
    ASSERT_TRUE(RegisterGamepadModule(registry, harness.Hooks(), options, options).succeeded());
    const auto* type = registry.FindVisualSource("org.exosnap.gamepad/controller");
    ASSERT_NE(type, nullptr);
    auto source = type->create(harness.Creation());
    ASSERT_TRUE(source.succeeded());
    EXPECT_EQ(harness.prepares, 0);
    const auto request = harness.Request();
    ASSERT_TRUE((*source.value())->Apply(request).succeeded());
    EXPECT_EQ(harness.prepares, 1);
    EXPECT_EQ(harness.last_token, request.token);
    EXPECT_TRUE(harness.settings.enabled);
    ASSERT_TRUE(harness.completion);
    harness.completion({request.token, SourcePhase::Ready, {}});
    ASSERT_EQ(harness.statuses.size(), 1u);
    EXPECT_EQ(harness.statuses.front().token, request.token);
}

TEST(VisualGamepadSource, RepeatedApplyAndAdditionalConsumersDoNotReprepare) {
    GamepadHarness harness;
    GamepadSource source(harness.Creation(), harness.Hooks());
    auto request = harness.Request();
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_TRUE(source.Apply(request).succeeded());
    request.demand.consumers = 7;
    EXPECT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(harness.prepares, 1);
    EXPECT_EQ(harness.stops, 0);
}

TEST(VisualGamepadSource, NoDemandDefersConfigurationAndReopenRequiresANewToken) {
    GamepadHarness harness;
    GamepadSource source(harness.Creation(), harness.Hooks());
    ASSERT_TRUE(source.Apply(harness.Request(1, 0)).succeeded());
    EXPECT_EQ(harness.prepares, 0);
    EXPECT_FALSE(source.Apply(harness.Request(1, 1)).succeeded());
    EXPECT_TRUE(source.Apply(harness.Request(2, 1)).succeeded());
    EXPECT_EQ(harness.prepares, 1);
    EXPECT_FALSE(source.Apply(harness.Request(2, 0)).succeeded());
    EXPECT_TRUE(source.Apply(harness.Request(3, 0)).succeeded());
    EXPECT_EQ(harness.stops, 1);
    EXPECT_FALSE(source.Apply(harness.Request(3, 1)).succeeded());
    EXPECT_TRUE(source.Apply(harness.Request(4, 1)).succeeded());
    EXPECT_EQ(harness.prepares, 2);
}

TEST(VisualGamepadSource, StaleAndRetokenedContentCannotReplaceCurrentPreparation) {
    GamepadHarness harness;
    GamepadSource source(harness.Creation(), harness.Hooks());
    auto changed = harness.Request();
    changed.validated_settings.insert_or_assign("layout", "dualsense");
    EXPECT_FALSE(source.Apply(changed).succeeded());
    auto request = harness.Request(2);
    request.validated_settings.insert_or_assign("layout", "dualsense");
    ASSERT_TRUE(source.Apply(request).succeeded());
    EXPECT_EQ(harness.settings.layout, 0);
    EXPECT_FALSE(source.Apply(harness.Request(1)).succeeded());
    EXPECT_FALSE(source.Apply(harness.Request(2)).succeeded());
    auto wrong_address = harness.Request(3);
    wrong_address.token.address.runtime.value = 2;
    EXPECT_FALSE(source.Apply(wrong_address).succeeded());
    EXPECT_EQ(harness.prepares, 1);
    EXPECT_EQ(harness.last_token, request.token);
}

TEST(VisualGamepadSource, StopIsIdempotentAndWaitsForExistingPreparationQuiescence) {
    GamepadHarness harness;
    GamepadSource source(harness.Creation(), harness.Hooks());
    ASSERT_TRUE(source.Apply(harness.Request()).succeeded());
    source.RequestStop();
    source.RequestStop();
    EXPECT_EQ(harness.stops, 1);
    EXPECT_FALSE(source.IsQuiescent());
    EXPECT_FALSE(source.Apply(harness.Request(2)).succeeded());
    harness.quiescent = true;
    EXPECT_TRUE(source.IsQuiescent());
    EXPECT_EQ(harness.prepares, 1);
}

TEST(VisualGamepadSource, InvalidSettingsAndIncompleteHooksDoNotStartWork) {
    GamepadHarness harness;
    GamepadSource source(harness.Creation(), harness.Hooks());
    auto request = harness.Request(2);
    request.validated_settings.insert_or_assign("layout", 2);
    EXPECT_FALSE(source.Apply(request).succeeded());
    EXPECT_EQ(harness.prepares, 0);
    CapabilityRegistry registry;
    const auto options = [](const VisualSourceRecord&) { return ChoiceSnapshot{}; };
    auto hooks = harness.Hooks();
    hooks.request_prepare = {};
    EXPECT_FALSE(RegisterGamepadModule(registry, std::move(hooks), options, options).succeeded());
    EXPECT_TRUE(registry.VisualSources().empty());
}

TEST(VisualGamepadSource, InertAdapterIsQuiescentWhenTheSharedOwnerIsBusy) {
    GamepadHarness harness;
    harness.quiescent = false;
    GamepadSource source(harness.Creation(), harness.Hooks());
    EXPECT_TRUE(source.IsQuiescent());
    source.RequestStop();
    EXPECT_TRUE(source.IsQuiescent());
    EXPECT_EQ(harness.prepares, 0);
    EXPECT_EQ(harness.stops, 0);
}

} // namespace
} // namespace exosnap
