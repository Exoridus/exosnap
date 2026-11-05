#include "extensions/CapabilityRegistry.h"
#include "models/VisualSceneConfig.h"

#include <gtest/gtest.h>

#include <limits>

namespace exosnap {
namespace {

VisualSourceType SettingsType() {
    VisualSourceType type;
    type.id = "test/image";
    type.label = {"VisualTests", "Image"};
    type.settings.validate = [](const toml::table& settings) -> engine::Outcome<toml::table> { return settings; };
    SettingDescriptor field;
    field.path = "device";
    field.label = {"VisualTests", "Device"};
    field.control = ControlKind::Select;
    field.default_value = std::string{};
    field.options_id = "devices";
    type.settings.fields.push_back(field);
    type.options.push_back({"devices", [](const VisualSourceRecord&) {
                                return ChoiceSnapshot{1,
                                                      {{"available", {"VisualTests", "Available"}},
                                                       {"missing", {"VisualTests", "Missing"}, false}}};
                            }});
    type.actions.push_back(
        {"select-device", {"VisualTests", "Select device"}, ActionOperation::SelectProperty, "device", "devices"});
    type.create = [](SourceCreateRequest) -> engine::Outcome<std::unique_ptr<VisualSource>> {
        return engine::EngineError{};
    };
    return type;
}

TEST(VisualSettings, MissingScopedOptionRegistrationIsRejected) {
    CapabilityRegistry registry;
    auto type = SettingsType();
    type.options.clear();
    EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
    EXPECT_TRUE(registry.VisualSources().empty());
}

TEST(VisualSettings, OptionIdsAreScopedToType) {
    CapabilityRegistry registry;
    auto first = SettingsType();
    auto second = SettingsType();
    second.id = "test/second";
    EXPECT_TRUE(registry.Register({{"test/module"}, {first, second}}).succeeded());
}

TEST(VisualSettings, DuplicateFieldsOptionsAndActionsAreRejected) {
    for (int duplicate = 0; duplicate != 3; ++duplicate) {
        CapabilityRegistry registry;
        auto type = SettingsType();
        if (duplicate == 0)
            type.settings.fields.push_back(type.settings.fields.front());
        else if (duplicate == 1)
            type.options.push_back(type.options.front());
        else
            type.actions.push_back(type.actions.front());
        EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
    }
}

TEST(VisualSettings, IdentifierUsesBoundedAsciiGrammar) {
    EXPECT_TRUE(IsVisualIdentifier("org.exosnap/device-2_option.v1"));
    EXPECT_TRUE(IsVisualIdentifier(std::string(128, 'a')));
    for (const auto& id : {std::string{}, std::string("/device"), std::string("a b"), std::string("A"),
                           std::string("a\\b"), std::string(129, 'a'), std::string("a\xFF")})
        EXPECT_FALSE(IsVisualIdentifier(id));
}

TEST(VisualSettings, InvalidRangeAndActionPropertyAreRejected) {
    CapabilityRegistry registry;
    auto type = SettingsType();
    type.settings.fields.front().minimum = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
    type = SettingsType();
    type.actions.front().property_path = "unknown";
    EXPECT_FALSE(registry.Register({{"test/module"}, {type}}).succeeded());
}

TEST(VisualSettings, ActionArgumentsChoiceAndIncarnationAreValidated) {
    const auto type = SettingsType();
    const engine::VisualAddress address{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{1}};
    const VisualSourceRecord source{address.source, type.id, 1, true, {}};
    ActionInvocation action{address, engine::CommandId{{2}}, "select-device", "available"};
    EXPECT_TRUE(ValidateVisualAction(action, type, source, address).succeeded());
    action.choice_id.reset();
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.choice_id = "missing";
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.choice_id = "unknown";
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.choice_id = "available";
    action.target.runtime.value = 2;
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.target = address;
    action.action_id = "unknown";
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
}

TEST(VisualSettings, RetryRejectsUnexpectedChoice) {
    auto type = SettingsType();
    type.actions.push_back({"retry", {"VisualTests", "Retry"}, ActionOperation::Retry, {}, {}});
    const engine::VisualAddress address{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{1}};
    const VisualSourceRecord source{address.source, type.id, 1, true, {}};
    ActionInvocation action{address, engine::CommandId{{2}}, "retry", "available"};
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.choice_id.reset();
    EXPECT_TRUE(ValidateVisualAction(action, type, source, address).succeeded());
}

TEST(VisualSettings, MalformedAndIncompatibleActionInputsAreNotStale) {
    const auto type = SettingsType();
    const engine::VisualAddress address{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{1}};
    VisualSourceRecord source{address.source, type.id, 1, true, {}};
    ActionInvocation action{address, engine::CommandId{{2}}, "select-device", "available"};
    const auto expect_invalid = [&] {
        auto result = ValidateVisualAction(action, type, source, address);
        ASSERT_FALSE(result.succeeded());
        EXPECT_EQ(result.error()->code, engine::EngineErrorCode::InvalidConfig);
    };
    action.request = {};
    expect_invalid();
    action.request = engine::CommandId{{2}};
    action.target.runtime = {};
    expect_invalid();
    action.target = address;
    action.action_id.clear();
    expect_invalid();
    action.action_id = "select-device";
    source.type_id = "test/other";
    expect_invalid();
    source.type_id = type.id;
    source.settings_version = 2;
    expect_invalid();
    source.settings_version = 1;
    action.target.runtime.value = 2;
    auto stale = ValidateVisualAction(action, type, source, address);
    ASSERT_FALSE(stale.succeeded());
    EXPECT_EQ(stale.error()->code, engine::EngineErrorCode::StaleRevision);
}

TEST(VisualSettings, DuplicateCurrentChoicesAreAmbiguousAndDetailsRejectsChoices) {
    auto type = SettingsType();
    type.options.front().snapshot = [](const VisualSourceRecord&) {
        return ChoiceSnapshot{1, {{"available", {"VisualTests", "First"}}, {"available", {"VisualTests", "Second"}}}};
    };
    type.actions.push_back({"details", {"VisualTests", "Details"}, ActionOperation::OpenDetails, {}, {}});
    const engine::VisualAddress address{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{1}};
    const VisualSourceRecord source{address.source, type.id, 1, true, {}};
    ActionInvocation action{address, engine::CommandId{{2}}, "select-device", "available"};
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.action_id = "details";
    EXPECT_FALSE(ValidateVisualAction(action, type, source, address).succeeded());
    action.choice_id.reset();
    EXPECT_TRUE(ValidateVisualAction(action, type, source, address).succeeded());
}

} // namespace
} // namespace exosnap
