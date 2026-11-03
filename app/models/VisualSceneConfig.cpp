#include "VisualSceneConfig.h"
#include "extensions/CapabilityRegistry.h"

#include <cmath>
#include <map>
#include <set>

namespace exosnap {

engine::Outcome<VisualSceneConfig> ValidateVisualScene(const VisualSceneConfig& scene,
                                                       const CapabilityRegistry& registry) {
    std::set<engine::VisualSourceId> sources;
    std::set<engine::SceneItemId> items;
    std::map<std::string_view, uint32_t> counts;
    for (const auto& source : scene.sources) {
        if (source.id == engine::VisualSourceId{} || !sources.insert(source.id).second ||
            !IsVisualIdentifier(source.type_id) || source.settings_version == 0)
            return engine::EngineError{.context = "Invalid or duplicate visual source identity"};
        if (const auto* type = registry.FindVisualSource(source.type_id)) {
            if (++counts[source.type_id] > type->instance_limit)
                return engine::EngineError{.context = "Visual source instance limit exceeded"};
        }
    }
    const auto normalized = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
    for (const auto& item : scene.items) {
        if (item.id == engine::SceneItemId{} || !items.insert(item.id).second || !sources.contains(item.source))
            return engine::EngineError{.context = "Invalid visual item identity or source reference"};
        const auto& placement = item.properties.placement;
        if (!normalized(placement.x) || !normalized(placement.y) || !normalized(placement.w) ||
            !normalized(placement.h) || placement.w < engine::WebcamPlacement::kMinSize ||
            placement.h < engine::WebcamPlacement::kMinSize || placement.x + placement.w > 1.000001f ||
            placement.y + placement.h > 1.000001f || !normalized(item.properties.opacity))
            return engine::EngineError{.context = "Invalid visual item geometry or opacity"};
        const auto& chroma = item.chroma_key;
        if (!normalized(chroma.tolerance) || !normalized(chroma.softness) || !normalized(chroma.spill_reduction) ||
            (chroma.color_mode != WebcamChromaKeyColorMode::Green &&
             chroma.color_mode != WebcamChromaKeyColorMode::Blue &&
             chroma.color_mode != WebcamChromaKeyColorMode::Magenta &&
             chroma.color_mode != WebcamChromaKeyColorMode::Custom))
            return engine::EngineError{.context = "Invalid visual item chroma settings"};
    }
    auto validated = scene;
    for (auto& source : validated.sources) {
        const auto* type = registry.FindVisualSource(source.type_id);
        if (!type || source.settings_version > type->settings_version)
            continue;
        auto settings = type->settings.validate(source.settings);
        if (!settings.succeeded())
            return *settings.error();
        source.settings = std::move(*settings.value());
    }
    return validated;
}

} // namespace exosnap
