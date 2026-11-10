#include "extensions/BuiltinVisualSettings.h"
#include "models/VisualSceneConfig.h"

#include <algorithm>
#include <gtest/gtest.h>

#include <limits>

namespace exosnap {
namespace {

SourceFactory InertFactory() {
    return [](SourceCreateRequest) -> engine::Outcome<std::unique_ptr<VisualSource>> { return engine::EngineError{}; };
}

CachedVisualOptions EmptyChoices() {
    return [](const VisualSourceRecord&) { return ChoiceSnapshot{}; };
}

TEST(BuiltinVisualSettings, GamepadRejectsUnsupportedDetailsAction) {
    const auto type = MakeGamepadVisualSourceType(InertFactory(), EmptyChoices(), EmptyChoices());
    const VisualSourceRecord source{engine::VisualSourceId{{1}}, type.id, 1, true, {}};
    const engine::VisualAddress address{source.id, engine::VisualRuntimeGeneration{7}};
    ActionInvocation invocation{
        .target = address, .request = engine::CommandId{{2}}, .action_id = "details", .item = engine::SceneItemId{{3}}};

    EXPECT_FALSE(ValidateVisualAction(invocation, type, source, address).succeeded());
    invocation.action_id = "retry";
    EXPECT_TRUE(ValidateVisualAction(invocation, type, source, address).succeeded());
}

TEST(BuiltinVisualSettings, WebcamDetailsRequiresCurrentRuntimeAndCompatibleSettings) {
    const auto type = MakeWebcamVisualSourceType(InertFactory(), EmptyChoices());
    VisualSourceRecord source{engine::VisualSourceId{{1}}, type.id, 1, true, {}};
    const engine::VisualAddress address{source.id, engine::VisualRuntimeGeneration{7}};
    ActionInvocation invocation{
        .target = address, .request = engine::CommandId{{2}}, .action_id = "details", .item = engine::SceneItemId{{3}}};

    EXPECT_TRUE(ValidateVisualAction(invocation, type, source, address).succeeded());
    invocation.target.runtime = engine::VisualRuntimeGeneration{6};
    EXPECT_FALSE(ValidateVisualAction(invocation, type, source, address).succeeded());
    invocation.target = address;
    source.settings_version = 2;
    EXPECT_FALSE(ValidateVisualAction(invocation, type, source, address).succeeded());
}

TEST(BuiltinVisualSettings, OnlyWebcamDeclaresTheSupportedDetailsDestination) {
    const auto gamepad = MakeGamepadVisualSourceType(InertFactory(), EmptyChoices(), EmptyChoices());
    const auto webcam = MakeWebcamVisualSourceType(InertFactory(), EmptyChoices());
    for (const auto& action : gamepad.actions)
        EXPECT_NE(action.operation, ActionOperation::OpenDetails);
    EXPECT_TRUE(std::any_of(webcam.actions.begin(), webcam.actions.end(),
                            [](const auto& action) { return action.operation == ActionOperation::OpenDetails; }));
}

TEST(BuiltinVisualSettings, AllGamepadLayoutsAndAutoProjectWithoutChangingUnknownNodes) {
    const auto type = MakeGamepadVisualSourceType(InertFactory(), EmptyChoices(), EmptyChoices());
    ASSERT_TRUE(type.settings.validate);
    for (int index = -1; index < static_cast<int>(kGamepadOverlayArtworkIds.size()); ++index) {
        const auto layout = index < 0 ? std::string_view("auto") : kGamepadOverlayArtworkIds[index];
        const toml::table input{{"device", std::string(64, 'a')},
                                {"layout", layout},
                                {"glass", true},
                                {"glow", true},
                                {"vendor", toml::table{{"future", 7}}}};
        auto projected = ProjectGamepadSettings(input);
        ASSERT_TRUE(projected.succeeded());
        EXPECT_EQ(projected.value()->layout, index);
        EXPECT_EQ(projected.value()->device, std::string(64, 'a'));
        EXPECT_FALSE(projected.value()->glass);
        EXPECT_FALSE(projected.value()->glow);
        auto validated = type.settings.validate(input);
        ASSERT_TRUE(validated.succeeded());
        EXPECT_EQ(*validated.value(), input);
    }
}

TEST(BuiltinVisualSettings, GamepadRejectsMalformedCurrentSettingsInsteadOfRepairingThem) {
    EXPECT_FALSE(ProjectGamepadSettings(toml::table{{"device", "bad"}}).succeeded());
    EXPECT_FALSE(ProjectGamepadSettings(toml::table{{"device", std::string(64, 'A')}}).succeeded());
    EXPECT_FALSE(ProjectGamepadSettings(toml::table{{"layout", 1}}).succeeded());
    EXPECT_FALSE(ProjectGamepadSettings(toml::table{{"layout", "not-a-layout"}}).succeeded());
    auto defaults = ProjectGamepadSettings({});
    ASSERT_TRUE(defaults.succeeded());
    EXPECT_EQ(defaults.value()->layout, -1);
    EXPECT_TRUE(defaults.value()->device.empty());
}

TEST(BuiltinVisualSettings, WebcamPreservesModeControlsAndUnknownSubtrees) {
    const toml::table input{
        {"device_id", "missing-camera"},
        {"width", 8192},
        {"height", 4320},
        {"fps", 120},
        {"controls", toml::table{{"exposure", toml::table{{"auto", false}, {"value", -6}, {"vendor", "keep"}}},
                                 {"brightness", toml::table{{"auto", true}, {"value", 11}}},
                                 {"future_control", toml::array{1, 2}}}},
        {"vendor", toml::table{{"future", "preserved"}}}};
    auto projected = ProjectWebcamSettings(input);
    ASSERT_TRUE(projected.succeeded());
    EXPECT_EQ(projected.value()->device_id, "missing-camera");
    EXPECT_EQ(projected.value()->width, 8192);
    EXPECT_EQ(projected.value()->height, 4320);
    EXPECT_EQ(projected.value()->fps, 120);
    ASSERT_EQ(projected.value()->controls.size(), 2u);
    EXPECT_EQ(projected.value()->controls[0], (WebcamControlSetting{WebcamControl::Brightness, true, 11}));
    EXPECT_EQ(projected.value()->controls[1], (WebcamControlSetting{WebcamControl::Exposure, false, -6}));
    const auto type = MakeWebcamVisualSourceType(InertFactory(), EmptyChoices());
    ASSERT_TRUE(type.settings.validate);
    auto validated = type.settings.validate(input);
    ASSERT_TRUE(validated.succeeded());
    EXPECT_EQ(*validated.value(), input);
}

TEST(BuiltinVisualSettings, WebcamRejectsWrongTypesAndOutOfDomainValues) {
    EXPECT_FALSE(ProjectWebcamSettings(toml::table{{"device_id", 1}}).succeeded());
    EXPECT_FALSE(ProjectWebcamSettings(toml::table{{"width", 0}}).succeeded());
    EXPECT_FALSE(ProjectWebcamSettings(toml::table{{"fps", 30.0}}).succeeded());
    EXPECT_FALSE(
        ProjectWebcamSettings(toml::table{{"height", int64_t(std::numeric_limits<int>::max()) + 1}}).succeeded());
    EXPECT_FALSE(ProjectWebcamSettings(toml::table{{"controls", "invalid"}}).succeeded());
    EXPECT_FALSE(ProjectWebcamSettings(
                     toml::table{{"controls", toml::table{{"exposure", toml::table{{"auto", 1}, {"value", -6}}}}}})
                     .succeeded());
    EXPECT_FALSE(
        ProjectWebcamSettings(
            toml::table{
                {"controls",
                 toml::table{{"exposure", toml::table{{"value", int64_t(std::numeric_limits<long>::max()) + 1}}}}}})
            .succeeded());
    auto defaults = ProjectWebcamSettings({});
    ASSERT_TRUE(defaults.succeeded());
    EXPECT_EQ(defaults.value()->width, 1280);
    EXPECT_TRUE(defaults.value()->device_id.empty());
    EXPECT_TRUE(defaults.value()->controls.empty());
}

TEST(BuiltinVisualSettings, MetadataUsesScopedCachedOptionsAndExistingChangePolicies) {
    int reads = 0;
    const auto cached = [&reads](const VisualSourceRecord&) {
        ++reads;
        return ChoiceSnapshot{1, {{"", {"BuiltinTests", "Automatic"}}}};
    };
    auto gamepad = MakeGamepadVisualSourceType(InertFactory(), cached, cached);
    auto webcam = MakeWebcamVisualSourceType(InertFactory(), cached);
    EXPECT_EQ(reads, 0);
    CapabilityRegistry registry;
    ASSERT_TRUE(registry.Register({{"org.exosnap.gamepad"}, {gamepad}}).succeeded());
    ASSERT_TRUE(registry.Register({{"org.exosnap.webcam"}, {webcam}}).succeeded());
    ASSERT_TRUE(registry.Seal().succeeded());
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(gamepad.id, "org.exosnap.gamepad/controller");
    EXPECT_EQ(webcam.id, "org.exosnap.webcam/camera");
    EXPECT_EQ(gamepad.instance_limit, 1u);
    EXPECT_EQ(webcam.instance_limit, 1u);
    ASSERT_EQ(gamepad.settings.fields.size(), 2u);
    ASSERT_EQ(webcam.settings.fields.size(), 1u);
    EXPECT_EQ(gamepad.settings.fields[0].change, ChangePolicy::Live);
    EXPECT_EQ(gamepad.settings.fields[1].change, ChangePolicy::Live);
    EXPECT_EQ(webcam.settings.fields[0].path, "device_id");
    EXPECT_EQ(webcam.settings.fields[0].change, ChangePolicy::NextSession);
    ASSERT_EQ(gamepad.options.size(), 2u);
    const VisualSourceRecord source{engine::VisualSourceId{{1}}, gamepad.id, 1, true, {}};
    EXPECT_EQ(gamepad.options[0].snapshot(source).choices.size(), 1u);
    EXPECT_EQ(reads, 1);
}

} // namespace
} // namespace exosnap
