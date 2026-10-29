#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace exosnap {

// Indices are persisted in profiles; append entries without reordering them.
inline constexpr std::array<std::string_view, 14> kGamepadOverlayArtworkIds{"dualsense",
                                                                            "xbox_series",
                                                                            "generic",
                                                                            "dualshock4",
                                                                            "xbox_one",
                                                                            "switch_pro",
                                                                            "joycon_grip",
                                                                            "joycon_pair",
                                                                            "joycon_left",
                                                                            "joycon_right",
                                                                            "joycon_left_horizontal",
                                                                            "joycon_right_horizontal",
                                                                            "steam_controller",
                                                                            "steam_deck_full"};

struct GamepadOverlaySettings {
    bool enabled = false;
    bool output_visible = true;
    std::string device;
    int layout = -1;
    bool glass = false;
    bool glow = false;
    float opacity = 1;
    float x = 0.70F;
    float y = 0.65F;
    float width = 0.25F;
    float height = 0.30F;

    bool operator==(const GamepadOverlaySettings&) const = default;
};

inline GamepadOverlaySettings SanitizeGamepadOverlaySettings(GamepadOverlaySettings value) {
    const auto finite = [](float number, float fallback, float low, float high) {
        return std::isfinite(number) ? std::clamp(number, low, high) : fallback;
    };
    value.layout = std::max(value.layout, -1);
    if (value.layout >= static_cast<int>(kGamepadOverlayArtworkIds.size()))
        value.layout = 2;
    value.opacity = finite(value.opacity, 1, 0, 1);
    value.width = finite(value.width, 0.25F, 0.05F, 1);
    value.height = finite(value.height, 0.30F, 0.05F, 1);
    value.x = finite(value.x, 0, 0, 1 - value.width);
    value.y = finite(value.y, 0, 0, 1 - value.height);
    if (value.device.size() != 64 || !std::all_of(value.device.begin(), value.device.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        value.device.clear();
    return value;
}

} // namespace exosnap
