#pragma once

#include <exosnap/engine/gamepad_sprite_data.h>
#include <gamepad/gamepad.h>

#include <QString>

namespace exosnap::quick {

using engine::GamepadSpritePart;
using engine::GamepadSpriteSet;
using engine::PadAuxiliary;
using engine::PadMotion;

/// Rasterizes immutable artwork at recording resolution. Call only when the
/// artwork, style or settled widget size changes, never from the video thread.
[[nodiscard]] std::shared_ptr<const GamepadSpriteSet> RasterizeGamepad(const QString& path, int width, bool glass,
                                                                       bool glow, QString& error);

/// Builds quads with body-only GPU masks while reusing the immutable atlas.
/// A radial display deadzone leaves the captured input values unchanged.
[[nodiscard]] std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadLayer(const GamepadSpriteSet& sprites,
                                                                              const gamepad::GamepadState& state,
                                                                              engine::SpriteContentRect placement,
                                                                              float opacity);

} // namespace exosnap::quick
