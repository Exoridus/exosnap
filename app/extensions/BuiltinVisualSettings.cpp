#include "BuiltinVisualSettings.h"

#include <QCoreApplication>

#include <algorithm>
#include <limits>

namespace exosnap {
namespace {

template <typename T> bool ReadExact(const toml::table& table, std::string_view key, T& destination) {
    const auto* node = table.get(key);
    if (!node)
        return true;
    const auto value = node->value_exact<T>();
    if (!value)
        return false;
    destination = *value;
    return true;
}

template <typename T> bool ReadInteger(const toml::table& table, std::string_view key, T& destination) {
    const auto* node = table.get(key);
    if (!node)
        return true;
    const auto* value = node->as_integer();
    if (!value || value->get() < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
        value->get() > static_cast<int64_t>(std::numeric_limits<T>::max()))
        return false;
    destination = static_cast<T>(value->get());
    return true;
}

} // namespace

engine::Outcome<GamepadOverlaySettings> ProjectGamepadSettings(const toml::table& table) {
    GamepadOverlaySettings settings;
    std::string layout = "auto";
    std::string view = "2d";
    if (!ReadExact(table, "device", settings.device) || !ReadExact(table, "layout", layout) ||
        !ReadExact(table, "view", view))
        return engine::EngineError{.context = "Gamepad device, layout and view must be strings"};
    if (view == "3d-normal")
        settings.view = GamepadView::NativeNormal;
    else if (view == "3d-ghost")
        settings.view = GamepadView::NativeGhost;
    else if (view != "2d")
        return engine::EngineError{.context = "Unknown gamepad view"};
    if (SanitizeGamepadOverlaySettings(settings).device != settings.device)
        return engine::EngineError{.context = "Invalid gamepad device identity"};
    if (layout != "auto") {
        const auto found = std::find(kGamepadOverlayArtworkIds.begin(), kGamepadOverlayArtworkIds.end(), layout);
        if (found == kGamepadOverlayArtworkIds.end())
            return engine::EngineError{.context = "Unknown gamepad layout"};
        settings.layout = static_cast<int>(found - kGamepadOverlayArtworkIds.begin());
    }
    return settings;
}

engine::Outcome<WebcamSettings> ProjectWebcamSettings(const toml::table& table) {
    WebcamSettings settings;
    if (!ReadExact(table, "device_id", settings.device_id) || !ReadInteger(table, "width", settings.width) ||
        !ReadInteger(table, "height", settings.height) || !ReadInteger(table, "fps", settings.fps) ||
        settings.width <= 0 || settings.height <= 0 || settings.fps <= 0)
        return engine::EngineError{.context = "Invalid webcam device or capture mode"};
    if (const auto* controls = table.get("controls")) {
        if (!controls->is_table())
            return engine::EngineError{.context = "Webcam controls must be a table"};
        for (const auto& [key, value] : *controls->as_table()) {
            const auto control = WebcamControlFromKey(key.str());
            if (!control)
                continue;
            WebcamControlSetting setting;
            setting.control = *control;
            if (!value.is_table() || !ReadExact(*value.as_table(), "auto", setting.automatic) ||
                !ReadInteger(*value.as_table(), "value", setting.value))
                return engine::EngineError{.context = "Invalid webcam control setting"};
            settings.controls.push_back(setting);
        }
        settings.controls = NormalizeWebcamControlSettings(std::move(settings.controls));
    }
    return settings;
}

VisualSourceType MakeGamepadVisualSourceType(SourceFactory factory, CachedVisualOptions devices,
                                             CachedVisualOptions layouts, CachedVisualOptions views) {
    VisualSourceType type;
    type.id = "org.exosnap.gamepad/controller";
    type.label = {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Gamepad")};
    type.settings.fields = {{.path = "device",
                             .label = {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Device")},
                             .control = ControlKind::Select,
                             .default_value = std::string{},
                             .options_id = "devices"},
                            {.path = "view",
                             .label = {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "View")},
                             .control = ControlKind::Select,
                             .default_value = std::string("2d"),
                             .options_id = "views"},
                            {.path = "layout",
                             .label = {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Layout")},
                             .control = ControlKind::Select,
                             .default_value = std::string("auto"),
                             .options_id = "layouts"}};
    type.settings.validate = [](const toml::table& settings) -> engine::Outcome<toml::table> {
        const auto projected = ProjectGamepadSettings(settings);
        if (!projected.succeeded())
            return *projected.error();
        return settings;
    };
    type.actions = {
        {"select-device",
         {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Select device")},
         ActionOperation::SelectProperty,
         "device",
         "devices"},
        {"select-layout",
         {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Select layout")},
         ActionOperation::SelectProperty,
         "layout",
         "layouts"},
        {"select-view",
         {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Select view")},
         ActionOperation::SelectProperty,
         "view",
         "views"},
        {"retry", {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Retry")}, ActionOperation::Retry, {}, {}}};
    if (!views)
        views = [](const VisualSourceRecord&) { return ChoiceSnapshot{}; };
    type.options = {{"devices", std::move(devices)}, {"layouts", std::move(layouts)}, {"views", std::move(views)}};
    type.create = std::move(factory);
    return type;
}

VisualSourceType MakeWebcamVisualSourceType(SourceFactory factory, CachedVisualOptions devices) {
    VisualSourceType type;
    type.id = "org.exosnap.webcam/camera";
    type.label = {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Webcam")};
    type.settings.fields = {{.path = "device_id",
                             .label = {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Device")},
                             .control = ControlKind::Select,
                             .default_value = std::string{},
                             .options_id = "devices",
                             .change = ChangePolicy::NextSession}};
    type.settings.validate = [](const toml::table& settings) -> engine::Outcome<toml::table> {
        const auto projected = ProjectWebcamSettings(settings);
        if (!projected.succeeded())
            return *projected.error();
        return settings;
    };
    type.actions = {
        {"select-device",
         {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Select device")},
         ActionOperation::SelectProperty,
         "device_id",
         "devices"},
        {"details",
         {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Details")},
         ActionOperation::OpenDetails,
         {},
         {}},
        {"retry", {"VisualSources", QT_TRANSLATE_NOOP("VisualSources", "Retry")}, ActionOperation::Retry, {}, {}}};
    type.options = {{"devices", std::move(devices)}};
    type.create = std::move(factory);
    return type;
}

} // namespace exosnap
