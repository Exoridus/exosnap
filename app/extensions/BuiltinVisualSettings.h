#pragma once

#include "CapabilityRegistry.h"
#include "models/GamepadOverlaySettings.h"
#include "models/WebcamSettings.h"

namespace exosnap {

// Return cached choices without enumerating or opening devices.
using CachedVisualOptions = std::function<ChoiceSnapshot(const VisualSourceRecord&)>;

// Project source/backend fields only. Presentation comes from host SceneItemProperties, not native DTO defaults.
[[nodiscard]] engine::Outcome<GamepadOverlaySettings> ProjectGamepadSettings(const toml::table&);
[[nodiscard]] engine::Outcome<WebcamSettings> ProjectWebcamSettings(const toml::table&);
[[nodiscard]] VisualSourceType MakeGamepadVisualSourceType(SourceFactory, CachedVisualOptions devices,
                                                           CachedVisualOptions layouts, CachedVisualOptions views = {});
[[nodiscard]] VisualSourceType MakeWebcamVisualSourceType(SourceFactory, CachedVisualOptions devices);

} // namespace exosnap
