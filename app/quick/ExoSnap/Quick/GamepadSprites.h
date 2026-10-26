#pragma once

#include <exosnap/engine/overlay_sprites.h>
#include <gamepad/gamepad.h>

#include <QString>

namespace exosnap::quick {

enum class PadMotion { None, LeftStick, RightStick, LeftTrigger, RightTrigger };
enum class PadAuxiliary { None, Home, Share, Touchpad };

struct GamepadSpritePart {
    engine::SpriteAtlasRect idle;
    engine::SpriteAtlasRect pressed;
    engine::SpriteContentRect bounds;
    uint32_t button = 0;
    PadMotion motion = PadMotion::None;
    PadAuxiliary auxiliary = PadAuxiliary::None;
    float travel_x = 8.0F / 480.0F;
    float travel_y = 8.0F / 320.0F;
};

struct GamepadSpriteSet {
    std::shared_ptr<const engine::SpriteAtlas> atlas;
    std::vector<GamepadSpritePart> parts;
};

/// Rasterizes immutable artwork at recording resolution. Call only when the
/// artwork, style or settled widget size changes, never from the video thread.
[[nodiscard]] std::shared_ptr<const GamepadSpriteSet> RasterizeGamepad(const QString& path, int width, bool glass,
                                                                       bool glow, QString& error);

/// Builds quads from normalized input without modifying or uploading the atlas.
/// A radial display deadzone leaves the captured input values unchanged.
[[nodiscard]] std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadLayer(const GamepadSpriteSet& sprites,
                                                                              const gamepad::GamepadState& state,
                                                                              engine::SpriteContentRect placement,
                                                                              float opacity);

} // namespace exosnap::quick
