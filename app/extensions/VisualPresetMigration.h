#pragma once

#include "models/GamepadOverlaySettings.h"
#include "models/VisualSceneConfig.h"

namespace exosnap {

// Built-in defaults have stable identities and do not create new source instances on hydration.
[[nodiscard]] VisualSceneConfig MakeDefaultVisualScene();
[[nodiscard]] engine::Outcome<VisualSceneConfig> MigrateLegacyVisualConfig(const WebcamSettings&,
                                                                           const GamepadOverlaySettings&);
// These codecs read and write the visual table. The preset store retains raw unknown record fields.
[[nodiscard]] engine::Outcome<VisualSceneConfig> ReadVisualScene(const toml::table&);
[[nodiscard]] engine::Outcome<toml::table> WriteVisualScene(const VisualSceneConfig&);

} // namespace exosnap
