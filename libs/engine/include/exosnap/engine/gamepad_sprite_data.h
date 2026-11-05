#pragma once

#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/recording_media_types.h>
#include <gamepad/gamepad.h>

namespace exosnap::engine {

enum class PadMotion { None, LeftStick, RightStick, LeftTrigger, RightTrigger };
enum class PadAuxiliary { None, Home, Share, Touchpad };

struct GamepadSpritePart {
    SpriteAtlasRect idle;
    SpriteAtlasRect pressed;
    SpriteContentRect bounds;
    uint32_t button = 0;
    PadMotion motion = PadMotion::None;
    PadAuxiliary auxiliary = PadAuxiliary::None;
    float travel_x = 8.0F / 480.0F;
    float travel_y = 8.0F / 320.0F;
    gamepad::Control control = gamepad::Control::Count;
    gamepad::Control click_control = gamepad::Control::Count;
    bool canonical = false;
    SpriteAtlasRect knockout;
    SpriteContentRect knockout_bounds;
};

struct GamepadSpriteSet {
    std::shared_ptr<const SpriteAtlas> atlas;
    std::vector<GamepadSpritePart> parts;
};

[[nodiscard]] size_t GamepadLayerQuadCapacity(const GamepadSpriteSet&) noexcept;
Outcome<std::monostate> PrepareGamepadLayer(const GamepadSpriteSet&, SpriteLayerSnapshot&);
// Output must have been prepared outside the frame loop. Returns whether its quads changed.
bool FillGamepadLayer(const GamepadSpriteSet&, const gamepad::GamepadState&, SpriteContentRect, float opacity,
                      SpriteLayerSnapshot&) noexcept;
[[nodiscard]] gamepad::GamepadState GamepadDisplayState(gamepad::GamepadState) noexcept;

} // namespace exosnap::engine
