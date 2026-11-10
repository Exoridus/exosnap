#include "VisualSceneEdits.h"

#include <algorithm>
#include <numeric>

namespace exosnap {
namespace {

bool SourceHasItems(const VisualSceneConfig& scene, engine::VisualSourceId source_id) {
    return std::any_of(scene.items.begin(), scene.items.end(),
                       [&](const auto& row) { return row.source == source_id; });
}

const SceneItemRecord* FindPilotItem(const VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                     engine::SceneItemId item_id, const SceneItemRecord& fallback) {
    const auto item = std::find_if(scene.items.begin(), scene.items.end(),
                                   [&](const auto& row) { return row.id == item_id && row.source == source_id; });
    if (item != scene.items.end())
        return &*item;
    return item_id == engine::SceneItemId{} && !SourceHasItems(scene, source_id) ? &fallback : nullptr;
}

SceneItemRecord& PilotItemForEdit(VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                  engine::SceneItemId item_id) {
    const auto item = std::find_if(scene.items.begin(), scene.items.end(),
                                   [&](const auto& row) { return row.id == item_id && row.source == source_id; });
    if (item != scene.items.end())
        return *item;
    SceneItemRecord restored;
    restored.id = engine::NewId<engine::SceneItemId>();
    restored.source = source_id;
    for (const auto& row : scene.items)
        restored.properties.order = std::max(restored.properties.order, row.properties.order + 1);
    return scene.items.emplace_back(std::move(restored));
}

SceneItemRecord DefaultWebcamItem() {
    const WebcamSettings defaults;
    SceneItemRecord item;
    item.properties.placement = {defaults.overlay.x_norm, defaults.overlay.y_norm, defaults.overlay.w_norm,
                                 defaults.overlay.h_norm, defaults.mirror};
    return item;
}

SceneItemRecord DefaultGamepadItem() {
    const GamepadOverlaySettings defaults;
    SceneItemRecord item;
    item.properties.placement = {defaults.x, defaults.y, defaults.width, defaults.height, false};
    item.properties.user_placed = true;
    return item;
}

// Publication draws items in this order, so it is the one stacking authority for preview and recording.
std::vector<size_t> StackOrder(const VisualSceneConfig& scene) {
    std::vector<size_t> order(scene.items.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return scene.items[a].properties.order < scene.items[b].properties.order;
    });
    return order;
}

std::vector<SceneItemRecord>::iterator FindOwnedItem(VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                                     engine::SceneItemId item_id) {
    const bool source_known =
        std::any_of(scene.sources.begin(), scene.sources.end(), [&](const auto& row) { return row.id == source_id; });
    if (!source_known || item_id == engine::SceneItemId{})
        return scene.items.end();
    return std::find_if(scene.items.begin(), scene.items.end(),
                        [&](const auto& row) { return row.id == item_id && row.source == source_id; });
}

engine::EngineError ForeignItem() {
    return engine::EngineError{.context = "Scene item does not belong to the addressed source"};
}

} // namespace

engine::Outcome<WebcamSettings> ProjectSceneWebcam(const VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                                   engine::SceneItemId item_id) {
    const auto source =
        std::find_if(scene.sources.begin(), scene.sources.end(), [&](const auto& row) { return row.id == source_id; });
    static const auto fallback = DefaultWebcamItem();
    const auto* item = FindPilotItem(scene, source_id, item_id, fallback);
    if (source == scene.sources.end() || !item || source->type_id != "org.exosnap.webcam/camera" ||
        source->settings_version != 1)
        return engine::EngineError{.context = "Webcam source or item is unavailable"};
    auto projected = ProjectWebcamSettings(source->settings);
    if (!projected.succeeded())
        return *projected.error();
    auto& settings = *projected.value();
    settings.enabled = source->enabled;
    const auto& properties = item->properties;
    const auto& placement = properties.placement;
    settings.output_visible = properties.output_visible;
    settings.overlay = {placement.x, placement.y, placement.w, placement.h};
    settings.overlay_user_placed = properties.user_placed;
    settings.aspect_ratio_locked = properties.aspect_ratio_locked;
    settings.mirror = placement.mirror;
    settings.opacity = properties.opacity;
    settings.chroma_key = item->chroma_key;
    return projected;
}

engine::Outcome<VisualSceneConfig> EditSceneWebcam(const VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                                   engine::SceneItemId item_id, const WebcamSettings& desired,
                                                   const CapabilityRegistry& registry) {
    if (!registry.IsSealed())
        return engine::EngineError{.context = "Visual registry is not sealed"};
    const auto before = ProjectSceneWebcam(scene, source_id, item_id);
    if (!before.succeeded())
        return *before.error();
    const auto* type = registry.FindVisualSource("org.exosnap.webcam/camera");
    if (!type || type->settings_version != 1)
        return engine::EngineError{.context = "Webcam settings are incompatible"};
    for (const auto& control : desired.controls) {
        if (WebcamControlKey(control.control).empty())
            return engine::EngineError{.context = "Unknown webcam control"};
    }
    auto candidate = scene;
    auto& source = *std::find_if(candidate.sources.begin(), candidate.sources.end(),
                                 [&](const auto& row) { return row.id == source_id; });
    auto& item = PilotItemForEdit(candidate, source_id, item_id);
    const auto& previous = *before.value();
    source.enabled = desired.enabled;
    if (previous.device_id != desired.device_id)
        source.settings.insert_or_assign("device_id", desired.device_id);
    if (previous.width != desired.width)
        source.settings.insert_or_assign("width", desired.width);
    if (previous.height != desired.height)
        source.settings.insert_or_assign("height", desired.height);
    if (previous.fps != desired.fps)
        source.settings.insert_or_assign("fps", desired.fps);
    if (previous.controls != desired.controls) {
        if (!source.settings.contains("controls"))
            source.settings.insert("controls", toml::table{});
        auto& controls = *source.settings["controls"].as_table();
        for (const auto& old : previous.controls) {
            if (!FindWebcamControlSetting(desired.controls, old.control))
                controls.erase(WebcamControlKey(old.control));
        }
        for (const auto& control : desired.controls) {
            const auto* old = FindWebcamControlSetting(previous.controls, control.control);
            if (old && *old == control)
                continue;
            const auto key = WebcamControlKey(control.control);
            if (!controls.contains(key))
                controls.insert(key, toml::table{});
            auto& value = *controls[key].as_table();
            value.insert_or_assign("auto", control.automatic);
            value.insert_or_assign("value", static_cast<int64_t>(control.value));
        }
    }
    auto& properties = item.properties;
    properties.output_visible = desired.output_visible;
    properties.user_placed = desired.overlay_user_placed;
    properties.aspect_ratio_locked = desired.aspect_ratio_locked;
    properties.opacity = desired.opacity;
    properties.placement = {desired.overlay.x_norm, desired.overlay.y_norm, desired.overlay.w_norm,
                            desired.overlay.h_norm, desired.mirror};
    item.chroma_key = desired.chroma_key;
    return ValidateVisualScene(candidate, registry);
}

engine::Outcome<GamepadOverlaySettings>
ProjectSceneGamepad(const VisualSceneConfig& scene, engine::VisualSourceId source_id, engine::SceneItemId item_id) {
    const auto source =
        std::find_if(scene.sources.begin(), scene.sources.end(), [&](const auto& row) { return row.id == source_id; });
    static const auto fallback = DefaultGamepadItem();
    const auto* item = FindPilotItem(scene, source_id, item_id, fallback);
    if (source == scene.sources.end() || !item || source->type_id != "org.exosnap.gamepad/controller" ||
        source->settings_version != 1)
        return engine::EngineError{.context = "Gamepad source or item is unavailable"};
    auto projected = ProjectGamepadSettings(source->settings);
    if (!projected.succeeded())
        return *projected.error();
    auto& settings = *projected.value();
    settings.enabled = source->enabled;
    const auto& properties = item->properties;
    settings.output_visible = properties.output_visible;
    settings.opacity = properties.opacity;
    settings.x = properties.placement.x;
    settings.y = properties.placement.y;
    settings.width = properties.placement.w;
    settings.height = properties.placement.h;
    return projected;
}

engine::Outcome<VisualSceneConfig> EditSceneGamepad(const VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                                    engine::SceneItemId item_id, const GamepadOverlaySettings& desired,
                                                    const CapabilityRegistry& registry) {
    if (!registry.IsSealed() || desired.layout < -1 ||
        desired.layout >= static_cast<int>(kGamepadOverlayArtworkIds.size()) || desired.glass || desired.glow ||
        (desired.view != GamepadView::TwoDimensional && desired.view != GamepadView::NativeNormal &&
         desired.view != GamepadView::NativeGhost))
        return engine::EngineError{.context = "Invalid gamepad edit"};
    const auto before = ProjectSceneGamepad(scene, source_id, item_id);
    const auto* type = registry.FindVisualSource("org.exosnap.gamepad/controller");
    if (!before.succeeded())
        return *before.error();
    if (!type || type->settings_version != 1)
        return engine::EngineError{.context = "Gamepad settings are incompatible"};
    auto candidate = scene;
    auto& source = *std::find_if(candidate.sources.begin(), candidate.sources.end(),
                                 [&](const auto& row) { return row.id == source_id; });
    auto& item = PilotItemForEdit(candidate, source_id, item_id);
    source.enabled = desired.enabled;
    if (before.value()->device != desired.device)
        source.settings.insert_or_assign("device", desired.device);
    if (before.value()->layout != desired.layout)
        source.settings.insert_or_assign("layout", desired.layout == -1 ? std::string_view("auto")
                                                                        : kGamepadOverlayArtworkIds[desired.layout]);
    if (before.value()->view != desired.view)
        source.settings.insert_or_assign("view", desired.view == GamepadView::NativeNormal  ? "3d-normal"
                                                 : desired.view == GamepadView::NativeGhost ? "3d-ghost"
                                                                                            : "2d");
    item.properties.output_visible = desired.output_visible;
    item.properties.opacity = desired.opacity;
    item.properties.placement.x = desired.x;
    item.properties.placement.y = desired.y;
    item.properties.placement.w = desired.width;
    item.properties.placement.h = desired.height;
    return ValidateVisualScene(candidate, registry);
}

engine::Outcome<VisualSceneConfig> MoveSceneItemToEdge(const VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                                       engine::SceneItemId item_id, SceneItemStackEdge edge,
                                                       const CapabilityRegistry& registry) {
    auto candidate = scene;
    const auto item = FindOwnedItem(candidate, source_id, item_id);
    if (item == candidate.items.end())
        return ForeignItem();
    const auto target = static_cast<size_t>(item - candidate.items.begin());
    auto order = StackOrder(candidate);
    std::erase(order, target);
    if (edge == SceneItemStackEdge::Front)
        order.push_back(target);
    else
        order.insert(order.begin(), target);
    for (size_t rank = 0; rank < order.size(); ++rank)
        candidate.items[order[rank]].properties.order = static_cast<int32_t>(rank);
    return ValidateVisualScene(candidate, registry);
}

engine::Outcome<VisualSceneConfig> SetSceneItemOutputVisible(const VisualSceneConfig& scene,
                                                             engine::VisualSourceId source_id,
                                                             engine::SceneItemId item_id, bool visible,
                                                             const CapabilityRegistry& registry) {
    auto candidate = scene;
    const auto item = FindOwnedItem(candidate, source_id, item_id);
    if (item == candidate.items.end())
        return ForeignItem();
    item->properties.output_visible = visible;
    return ValidateVisualScene(candidate, registry);
}

engine::Outcome<VisualSceneConfig> RemoveSceneItem(const VisualSceneConfig& scene, engine::VisualSourceId source_id,
                                                   engine::SceneItemId item_id, const CapabilityRegistry& registry) {
    auto candidate = scene;
    const auto item = FindOwnedItem(candidate, source_id, item_id);
    if (item == candidate.items.end())
        return ForeignItem();
    candidate.items.erase(item);
    if (!SourceHasItems(candidate, source_id)) {
        for (auto& source : candidate.sources) {
            if (source.id == source_id)
                source.enabled = false;
        }
    }
    return ValidateVisualScene(candidate, registry);
}

std::optional<size_t> SceneItemStackIndex(const VisualSceneConfig& scene, engine::SceneItemId item_id) {
    const auto order = StackOrder(scene);
    const auto found =
        std::find_if(order.begin(), order.end(), [&](size_t index) { return scene.items[index].id == item_id; });
    if (found == order.end())
        return std::nullopt;
    return static_cast<size_t>(found - order.begin());
}

} // namespace exosnap
