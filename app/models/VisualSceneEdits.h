#pragma once

#include "VisualSceneConfig.h"
#include "extensions/BuiltinVisualSettings.h"

namespace exosnap {

// A null item id addresses a source whose last item was removed: projection reports default presentation and an
// edit creates one new item, so enabling the source again restores it on top of the scene.
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

enum class SceneItemStackEdge { Front, Back };

// Host-owned node operations. Each addresses exactly one item of `source` and returns a validated scene. Reordering
// renumbers every item densely in its current stacking order, so ties and gaps cannot make the result ambiguous.
[[nodiscard]] engine::Outcome<VisualSceneConfig> MoveSceneItemToEdge(const VisualSceneConfig&, engine::VisualSourceId,
                                                                     engine::SceneItemId, SceneItemStackEdge,
                                                                     const CapabilityRegistry&);
[[nodiscard]] engine::Outcome<VisualSceneConfig> SetSceneItemOutputVisible(const VisualSceneConfig&,
                                                                           engine::VisualSourceId, engine::SceneItemId,
                                                                           bool visible, const CapabilityRegistry&);
// Removing the last item of a source also disables the source, so the source manager retires its runtime through
// the normal lifecycle. The source record and its settings remain for a later re-add.
[[nodiscard]] engine::Outcome<VisualSceneConfig> RemoveSceneItem(const VisualSceneConfig&, engine::VisualSourceId,
                                                                 engine::SceneItemId, const CapabilityRegistry&);
// Stacking position of an item among all items, 0 being the back. Empty when the item is absent.
[[nodiscard]] std::optional<size_t> SceneItemStackIndex(const VisualSceneConfig&, engine::SceneItemId);

} // namespace exosnap
