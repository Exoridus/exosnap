#include "VisualPresetMigration.h"
#include "CapabilityRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace exosnap {
namespace {

constexpr std::array<std::string_view, 4> kChromaModes{"green", "blue", "magenta", "custom"};
using Identities = std::array<std::array<uint8_t, 16>, 4>;

VisualSceneConfig BuildScene(WebcamSettings camera, GamepadOverlaySettings pad, const Identities& ids) {
    camera = SanitizeWebcamSettings(std::move(camera));
    pad = SanitizeGamepadOverlaySettings(std::move(pad));
    toml::table controls;
    for (const auto& control : camera.controls) {
        const auto key = WebcamControlKey(control.control);
        if (!key.empty())
            controls.insert(std::string(key),
                            toml::table{{"auto", control.automatic}, {"value", static_cast<int64_t>(control.value)}});
    }
    VisualSceneConfig scene;
    scene.sources.push_back({engine::VisualSourceId{ids[0]}, "org.exosnap.webcam/camera", 1, camera.enabled,
                             toml::table{{"device_id", camera.device_id},
                                         {"width", camera.width},
                                         {"height", camera.height},
                                         {"fps", camera.fps},
                                         {"controls", std::move(controls)}}});
    scene.sources.push_back(
        {engine::VisualSourceId{ids[1]}, "org.exosnap.gamepad/controller", 1, pad.enabled,
         toml::table{{"device", pad.device},
                     {"layout", pad.layout < 0 ? std::string_view("auto") : kGamepadOverlayArtworkIds[pad.layout]}}});
    SceneItemRecord camera_item;
    camera_item.id = engine::SceneItemId{ids[2]};
    camera_item.source = scene.sources[0].id;
    camera_item.properties.placement = {camera.overlay.x_norm, camera.overlay.y_norm, camera.overlay.w_norm,
                                        camera.overlay.h_norm, camera.mirror};
    camera_item.properties.opacity = camera.opacity;
    camera_item.properties.output_visible = camera.output_visible;
    camera_item.properties.aspect_ratio_locked = camera.aspect_ratio_locked;
    camera_item.properties.user_placed = camera.overlay_user_placed;
    camera_item.chroma_key = camera.chroma_key;
    SceneItemRecord pad_item;
    pad_item.id = engine::SceneItemId{ids[3]};
    pad_item.source = scene.sources[1].id;
    pad_item.properties.placement = {pad.x, pad.y, pad.width, pad.height, false};
    pad_item.properties.opacity = pad.opacity;
    pad_item.properties.output_visible = pad.output_visible;
    pad_item.properties.user_placed = true;
    pad_item.properties.order = 1;
    scene.items.push_back(camera_item);
    scene.items.push_back(pad_item);
    return scene;
}

template <typename T> bool ReadOptional(const toml::table& table, std::string_view key, T& destination) {
    const auto* node = table.get(key);
    if (!node)
        return true;
    const auto value = std::is_same_v<T, bool> ? node->value_exact<T>() : node->value<T>();
    if (!value)
        return false;
    destination = *value;
    return true;
}

bool ReadFloat(const toml::table& table, std::string_view key, float& destination) {
    double value = destination;
    if (!ReadOptional(table, key, value) || !std::isfinite(value) ||
        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    destination = static_cast<float>(value);
    return true;
}

template <typename T> bool ReadInteger(const toml::table& table, std::string_view key, T& destination) {
    const auto* node = table.get(key);
    if (!node)
        return true;
    const auto* integer = node->as_integer();
    if (!integer || integer->get() < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
        integer->get() > static_cast<int64_t>(std::numeric_limits<T>::max()))
        return false;
    destination = static_cast<T>(integer->get());
    return true;
}

bool ReadChroma(const toml::table& table, WebcamChromaKeySettings& chroma) {
    std::string mode(kChromaModes[static_cast<size_t>(chroma.color_mode)]);
    if (!ReadOptional(table, "enabled", chroma.enabled) || !ReadOptional(table, "color_mode", mode) ||
        !ReadInteger(table, "custom_r", chroma.custom_r) || !ReadInteger(table, "custom_g", chroma.custom_g) ||
        !ReadInteger(table, "custom_b", chroma.custom_b) || !ReadFloat(table, "tolerance", chroma.tolerance) ||
        !ReadFloat(table, "softness", chroma.softness) || !ReadFloat(table, "spill", chroma.spill_reduction))
        return false;
    const auto found = std::find(kChromaModes.begin(), kChromaModes.end(), mode);
    if (found == kChromaModes.end())
        return false;
    chroma.color_mode = static_cast<WebcamChromaKeyColorMode>(found - kChromaModes.begin());
    return true;
}

bool ReadSource(const toml::table& table, VisualSourceRecord& source) {
    const auto id = table["id"].value<std::string>();
    const auto parsed = id ? engine::FromUuid<engine::VisualSourceId>(*id) : std::nullopt;
    if (!parsed || !ReadOptional(table, "type_id", source.type_id) || !table.contains("settings_version") ||
        !ReadInteger(table, "settings_version", source.settings_version) ||
        !ReadOptional(table, "enabled", source.enabled))
        return false;
    source.id = *parsed;
    if (const auto* settings = table.get("settings")) {
        if (!settings->is_table())
            return false;
        source.settings = *settings->as_table();
    }
    return true;
}

bool ReadItem(const toml::table& table, SceneItemRecord& item) {
    const auto id = table["id"].value<std::string>();
    const auto source = table["source_id"].value<std::string>();
    const auto parsed_id = id ? engine::FromUuid<engine::SceneItemId>(*id) : std::nullopt;
    const auto parsed_source = source ? engine::FromUuid<engine::VisualSourceId>(*source) : std::nullopt;
    if (!parsed_id || !parsed_source)
        return false;
    item.id = *parsed_id;
    item.source = *parsed_source;
    auto& properties = item.properties;
    auto& placement = properties.placement;
    if (!ReadFloat(table, "x", placement.x) || !ReadFloat(table, "y", placement.y) ||
        !ReadFloat(table, "width", placement.w) || !ReadFloat(table, "height", placement.h) ||
        !ReadOptional(table, "mirror", placement.mirror) || !ReadFloat(table, "opacity", properties.opacity) ||
        !ReadOptional(table, "output_visible", properties.output_visible) ||
        !ReadOptional(table, "locked", properties.locked) ||
        !ReadOptional(table, "aspect_ratio_locked", properties.aspect_ratio_locked) ||
        !ReadOptional(table, "user_placed", properties.user_placed) || !ReadInteger(table, "order", properties.order))
        return false;
    if (const auto* chroma = table.get("chroma_key")) {
        if (!chroma->is_table() || !ReadChroma(*chroma->as_table(), item.chroma_key))
            return false;
    }
    return true;
}

} // namespace

VisualSceneConfig MakeDefaultVisualScene() {
    constexpr Identities ids{{{1, 0, 0, 0, 0, 0, 0x40, 0, 0x80},
                              {2, 0, 0, 0, 0, 0, 0x40, 0, 0x80},
                              {3, 0, 0, 0, 0, 0, 0x40, 0, 0x80},
                              {4, 0, 0, 0, 0, 0, 0x40, 0, 0x80}}};
    return BuildScene({}, {}, ids);
}

engine::Outcome<VisualSceneConfig> MigrateLegacyVisualConfig(const WebcamSettings& camera,
                                                             const GamepadOverlaySettings& pad) {
    const Identities ids{engine::NewIdentity(), engine::NewIdentity(), engine::NewIdentity(), engine::NewIdentity()};
    return ValidateVisualScene(BuildScene(camera, pad, ids), CapabilityRegistry{});
}

engine::Outcome<VisualSceneConfig> ReadVisualScene(const toml::table& table) {
    VisualSceneConfig scene;
    if (const auto* sources = table.get("sources")) {
        if (!sources->is_array())
            return engine::EngineError{.context = "Visual sources must be an array"};
        for (const auto& node : *sources->as_array()) {
            VisualSourceRecord source;
            if (!node.is_table() || !ReadSource(*node.as_table(), source))
                return engine::EngineError{.context = "Invalid visual source record"};
            scene.sources.push_back(std::move(source));
        }
    }
    if (const auto* items = table.get("items")) {
        if (!items->is_array())
            return engine::EngineError{.context = "Visual items must be an array"};
        for (const auto& node : *items->as_array()) {
            SceneItemRecord item;
            if (!node.is_table() || !ReadItem(*node.as_table(), item))
                return engine::EngineError{.context = "Invalid visual item record"};
            scene.items.push_back(std::move(item));
        }
    }
    return ValidateVisualScene(scene, CapabilityRegistry{});
}

engine::Outcome<toml::table> WriteVisualScene(const VisualSceneConfig& scene) {
    auto validated = ValidateVisualScene(scene, CapabilityRegistry{});
    if (!validated.succeeded())
        return *validated.error();
    toml::array sources;
    for (const auto& source : scene.sources) {
        sources.push_back(toml::table{{"id", engine::ToUuid(source.id)},
                                      {"type_id", source.type_id},
                                      {"settings_version", static_cast<int64_t>(source.settings_version)},
                                      {"enabled", source.enabled},
                                      {"settings", source.settings}});
    }
    toml::array items;
    for (const auto& item : scene.items) {
        const auto& properties = item.properties;
        const auto& placement = properties.placement;
        const auto& chroma = item.chroma_key;
        items.push_back(
            toml::table{{"id", engine::ToUuid(item.id)},
                        {"source_id", engine::ToUuid(item.source)},
                        {"x", static_cast<double>(placement.x)},
                        {"y", static_cast<double>(placement.y)},
                        {"width", static_cast<double>(placement.w)},
                        {"height", static_cast<double>(placement.h)},
                        {"mirror", placement.mirror},
                        {"opacity", static_cast<double>(properties.opacity)},
                        {"output_visible", properties.output_visible},
                        {"locked", properties.locked},
                        {"aspect_ratio_locked", properties.aspect_ratio_locked},
                        {"user_placed", properties.user_placed},
                        {"order", static_cast<int64_t>(properties.order)},
                        {"chroma_key", toml::table{{"enabled", chroma.enabled},
                                                   {"color_mode", kChromaModes[static_cast<size_t>(chroma.color_mode)]},
                                                   {"custom_r", static_cast<int64_t>(chroma.custom_r)},
                                                   {"custom_g", static_cast<int64_t>(chroma.custom_g)},
                                                   {"custom_b", static_cast<int64_t>(chroma.custom_b)},
                                                   {"tolerance", static_cast<double>(chroma.tolerance)},
                                                   {"softness", static_cast<double>(chroma.softness)},
                                                   {"spill", static_cast<double>(chroma.spill_reduction)}}}});
    }
    return toml::table{{"sources", std::move(sources)}, {"items", std::move(items)}};
}

} // namespace exosnap
