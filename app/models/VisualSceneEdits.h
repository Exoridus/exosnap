#pragma once

#include "VisualSceneConfig.h"
#include "extensions/BuiltinVisualSettings.h"

namespace exosnap {

[[nodiscard]] engine::Outcome<WebcamSettings> ProjectSceneWebcam(const VisualSceneConfig&, engine::VisualSourceId,
                                                                 engine::SceneItemId);
// Applies only changed known values. Unchanged absent values and unknown settings remain untouched.
[[nodiscard]] engine::Outcome<VisualSceneConfig> EditSceneWebcam(const VisualSceneConfig&, engine::VisualSourceId,
                                                                 engine::SceneItemId, const WebcamSettings&,
                                                                 const CapabilityRegistry&);
[[nodiscard]] engine::Outcome<GamepadOverlaySettings> ProjectSceneGamepad(const VisualSceneConfig&,
                                                                          engine::VisualSourceId, engine::SceneItemId);
[[nodiscard]] engine::Outcome<VisualSceneConfig> EditSceneGamepad(const VisualSceneConfig&, engine::VisualSourceId,
                                                                  engine::SceneItemId, const GamepadOverlaySettings&,
                                                                  const CapabilityRegistry&);

} // namespace exosnap
