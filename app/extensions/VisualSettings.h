#pragma once

#include <exosnap/engine/visual_scene.h>
#include <toml++/toml.h>

#include <functional>

namespace exosnap {

struct VisualSourceRecord;
struct VisualSourceType;

using SettingValue = std::variant<bool, int64_t, double, std::string>;
enum class ControlKind { CheckBox, Select, SliderNumber, Status };
enum class ChangePolicy { Live, NextSession };

struct LocalizedText {
    std::string context, source_text;
};

struct SettingDescriptor {
    std::string path;
    LocalizedText label;
    ControlKind control = ControlKind::Status;
    SettingValue default_value;
    std::optional<double> minimum, maximum, step;
    std::optional<uint32_t> max_length;
    std::string options_id;
    ChangePolicy change = ChangePolicy::Live;
};

struct Choice {
    std::string id;
    LocalizedText label;
    bool selectable = true;
    std::optional<LocalizedText> reason;
};

struct ChoiceSnapshot {
    uint64_t revision = 0;
    std::vector<Choice> choices;
};

struct OptionsRegistration {
    std::string id;
    std::function<ChoiceSnapshot(const VisualSourceRecord&)> snapshot;
};

struct SettingsDefinition {
    std::vector<SettingDescriptor> fields;
    // Validate known fields and preserve unknown keys and subtrees in the returned table.
    std::function<engine::Outcome<toml::table>(const toml::table&)> validate;
};

enum class ActionOperation { SelectProperty, OpenDetails, Retry };

struct ActionDescriptor {
    std::string id;
    LocalizedText label;
    ActionOperation operation = ActionOperation::OpenDetails;
    std::string property_path, options_id;
    // Offers the action in the host context menu of each scene item of this source. The host owns the menu layout,
    // places these entries before its own node operations and shows at most kMaxNodeMenuActions of them.
    bool node_menu = false;
};

inline constexpr size_t kMaxNodeMenuActions = 4;
// A node-menu selection lists at most this many choices; the current value is always kept.
inline constexpr size_t kMaxNodeMenuChoices = 16;

struct ActionInvocation {
    engine::VisualAddress target;
    engine::CommandId request;
    std::string action_id;
    std::optional<std::string> choice_id;
    // The scene item the action was offered for. The host rejects an invocation whose item does not belong to the
    // target source in the current scene.
    engine::SceneItemId item;
};

[[nodiscard]] bool IsVisualIdentifier(std::string_view) noexcept;
[[nodiscard]] engine::Outcome<std::monostate> ValidateVisualSettings(const VisualSourceType&);
[[nodiscard]] engine::Outcome<std::monostate> ValidateVisualAction(const ActionInvocation&, const VisualSourceType&,
                                                                   const VisualSourceRecord&,
                                                                   const engine::VisualAddress&);

} // namespace exosnap
