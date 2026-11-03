#include "CapabilityRegistry.h"
#include "models/VisualSceneConfig.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace exosnap {

bool IsVisualIdentifier(std::string_view id) noexcept {
    const auto alphanumeric = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return !id.empty() && id.size() <= 128 && alphanumeric(id.front()) &&
           std::all_of(id.begin(), id.end(),
                       [&](char c) { return alphanumeric(c) || c == '.' || c == '_' || c == '/' || c == '-'; });
}

engine::Outcome<std::monostate> ValidateVisualSettings(const VisualSourceType& type) {
    if (!type.settings.validate)
        return engine::EngineError{.context = "Missing visual settings validator"};
    std::set<std::string_view> options;
    for (const auto& option : type.options) {
        if (!IsVisualIdentifier(option.id) || !option.snapshot || !options.insert(option.id).second)
            return engine::EngineError{.context = "Invalid or duplicate visual options registration"};
    }
    std::set<std::string_view> fields;
    for (const auto& field : type.settings.fields) {
        if (!IsVisualIdentifier(field.path) || field.label.context.empty() || field.label.source_text.empty() ||
            !fields.insert(field.path).second)
            return engine::EngineError{.context = "Invalid or duplicate visual settings field"};
        const auto finite = [](std::optional<double> value) { return !value || std::isfinite(*value); };
        if (!finite(field.minimum) || !finite(field.maximum) || !finite(field.step) ||
            (field.minimum && field.maximum && *field.minimum > *field.maximum) || (field.step && *field.step <= 0))
            return engine::EngineError{.context = "Invalid visual setting range"};
        std::optional<double> number;
        if (const auto* integer = std::get_if<int64_t>(&field.default_value))
            number = static_cast<double>(*integer);
        else if (const auto* real = std::get_if<double>(&field.default_value))
            number = *real;
        if ((number && (!std::isfinite(*number) || (field.minimum && *number < *field.minimum) ||
                        (field.maximum && *number > *field.maximum))) ||
            (!number && (field.minimum || field.maximum || field.step)))
            return engine::EngineError{.context = "Invalid visual setting numeric default"};
        if (field.max_length) {
            const auto* text = std::get_if<std::string>(&field.default_value);
            if (!text || text->size() > *field.max_length)
                return engine::EngineError{.context = "Invalid visual setting text default"};
        }
        if (field.change != ChangePolicy::Live && field.change != ChangePolicy::NextSession)
            return engine::EngineError{.context = "Invalid visual setting change policy"};
        switch (field.control) {
        case ControlKind::CheckBox:
            if (!std::holds_alternative<bool>(field.default_value))
                return engine::EngineError{.context = "A checkbox requires a boolean default"};
            break;
        case ControlKind::Select:
            if (!std::holds_alternative<std::string>(field.default_value) || !options.contains(field.options_id))
                return engine::EngineError{.context = "A selection requires registered options and a string default"};
            break;
        case ControlKind::SliderNumber:
            if (!number)
                return engine::EngineError{.context = "A numeric setting requires a numeric default"};
            break;
        case ControlKind::Status:
            break;
        default:
            return engine::EngineError{.context = "Invalid visual settings control"};
        }
        if (field.control != ControlKind::Select && !field.options_id.empty())
            return engine::EngineError{.context = "Only selection fields accept options"};
    }
    std::set<std::string_view> actions;
    for (const auto& action : type.actions) {
        if (!IsVisualIdentifier(action.id) || action.label.context.empty() || action.label.source_text.empty() ||
            !actions.insert(action.id).second)
            return engine::EngineError{.context = "Invalid or duplicate visual action"};
        switch (action.operation) {
        case ActionOperation::SelectProperty: {
            const auto field =
                std::find_if(type.settings.fields.begin(), type.settings.fields.end(),
                             [&](const auto& candidate) { return candidate.path == action.property_path; });
            if (field == type.settings.fields.end() || field->control != ControlKind::Select ||
                field->options_id != action.options_id || !options.contains(action.options_id))
                return engine::EngineError{.context = "Visual action refers to an unregistered selection"};
            break;
        }
        case ActionOperation::OpenDetails:
        case ActionOperation::Retry:
            if (!action.property_path.empty() || !action.options_id.empty())
                return engine::EngineError{.context = "Visual action does not accept a property or options"};
            break;
        default:
            return engine::EngineError{.context = "Invalid visual action operation"};
        }
    }
    return std::monostate{};
}

engine::Outcome<std::monostate> ValidateVisualAction(const ActionInvocation& invocation, const VisualSourceType& type,
                                                     const VisualSourceRecord& source, engine::VisualAddress current) {
    if (invocation.target != current || current.runtime.value == 0 || current.source == engine::VisualSourceId{} ||
        source.id != current.source || source.type_id != type.id || source.settings_version != type.settings_version ||
        invocation.request == engine::CommandId{})
        return engine::EngineError{.code = engine::EngineErrorCode::StaleRevision,
                                   .context = "Visual action does not address the current editable source"};
    const auto action = std::find_if(type.actions.begin(), type.actions.end(),
                                     [&](const auto& candidate) { return candidate.id == invocation.action_id; });
    if (action == type.actions.end())
        return engine::EngineError{.context = "Unknown visual action"};
    if (action->operation != ActionOperation::SelectProperty) {
        if (invocation.choice_id ||
            (action->operation != ActionOperation::OpenDetails && action->operation != ActionOperation::Retry))
            return engine::EngineError{.context = "Visual action does not accept a choice"};
        return std::monostate{};
    }
    if (!invocation.choice_id)
        return engine::EngineError{.context = "Visual selection requires a choice"};
    const auto options = std::find_if(type.options.begin(), type.options.end(),
                                      [&](const auto& candidate) { return candidate.id == action->options_id; });
    if (options == type.options.end() || !options->snapshot)
        return engine::EngineError{.context = "Visual selection options are unavailable"};
    const auto snapshot = options->snapshot(source);
    const Choice* selected = nullptr;
    std::set<std::string_view> choices;
    for (const auto& choice : snapshot.choices) {
        if (!choices.insert(choice.id).second)
            return engine::EngineError{.context = "Visual selection options contain duplicate choices"};
        if (choice.id == *invocation.choice_id)
            selected = &choice;
    }
    if (!selected || !selected->selectable)
        return engine::EngineError{.context = "Visual selection choice is unavailable"};
    return std::monostate{};
}

} // namespace exosnap
