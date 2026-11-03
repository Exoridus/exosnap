#pragma once

#include "WebcamSettings.h"

#include <exosnap/engine/visual_scene.h>
#include <toml++/toml.h>

namespace exosnap {

class CapabilityRegistry;

struct VisualSourceRecord {
    engine::VisualSourceId id;
    std::string type_id;
    uint32_t settings_version = 1;
    bool enabled = false;
    toml::table settings;
};

struct SceneItemRecord {
    engine::SceneItemId id;
    engine::VisualSourceId source;
    engine::SceneItemProperties properties;
    WebcamChromaKeySettings chroma_key;
};

struct VisualSceneConfig {
    std::vector<VisualSourceRecord> sources;
    std::vector<SceneItemRecord> items;
};

[[nodiscard]] engine::Outcome<VisualSceneConfig> ValidateVisualScene(const VisualSceneConfig&,
                                                                     const CapabilityRegistry&);

} // namespace exosnap
