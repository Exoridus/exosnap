#include "gamepad/sdl_gamepad_state.h"

#include <SDL3/SDL_gamepad.h>
// Windows types must be defined before the BCrypt declarations.
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <limits>

namespace exosnap::gamepad {

LayoutKind SdlLayoutKindFor(SDL_GamepadType type, uint16_t vendor, uint16_t product) noexcept {
    const auto known = LayoutKindFor(vendor, product);
    if (known != LayoutKind::Generic)
        return known;
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS4:
        return LayoutKind::DualShock4;
    case SDL_GAMEPAD_TYPE_PS5:
        return LayoutKind::DualSense;
    case SDL_GAMEPAD_TYPE_XBOX360:
        return LayoutKind::Xbox360;
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return LayoutKind::XboxOne;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        return LayoutKind::SwitchPro;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        return LayoutKind::JoyConLeft;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        return LayoutKind::JoyConRight;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return LayoutKind::JoyConPair;
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return LayoutKind::GameCube;
    default:
        return LayoutKind::Generic;
    }
}

GamepadState NormalizeSdlControls(const SdlGamepadSample& sample) noexcept {
    static_assert(SDL_GAMEPAD_BUTTON_COUNT == 26 && SDL_GAMEPAD_AXIS_COUNT == 6);
    static_assert(SDL_GAMEPAD_BUTTON_SOUTH == static_cast<int>(Control::South) &&
                  SDL_GAMEPAD_BUTTON_MISC6 == static_cast<int>(Control::Misc6) &&
                  SDL_GAMEPAD_BUTTON_TOUCHPAD == static_cast<int>(Control::Touchpad));
    GamepadState state;
    state.controls_normalized = true;
    constexpr std::array<uint32_t, 26> legacy_masks{
        4,   8,   16, 32,         2,          0,          1,          4096, 8192, 1024, 2048, 64, 128,
        256, 512, 0,  0x10000000, 0x04000000, 0x20000000, 0x08000000, 0,    0,    0,    0,    0,  0};
    for (size_t index = 0; index < legacy_masks.size(); ++index) {
        if ((sample.supported_buttons & (1U << index)) == 0)
            continue;
        auto& control = state.controls[index];
        if (!sample.available) {
            control.status = ControlStatus::Unavailable;
        } else {
            const bool pressed = (sample.buttons & (1U << index)) != 0;
            control = {pressed ? ControlStatus::Active : ControlStatus::Released, pressed ? 1.0F : 0.0F, 0};
            if (pressed)
                state.buttons |= legacy_masks[index];
        }
    }
    const auto axis = [&](size_t index) {
        if (!sample.available || (sample.supported_axes & (1U << index)) == 0)
            return 0.0F;
        return static_cast<float>(sample.axes[index]) / (sample.axes[index] < 0 ? 32768.0F : 32767.0F);
    };
    state.left_x = axis(0);
    state.left_y = -axis(1);
    state.right_x = axis(2);
    state.right_y = -axis(3);
    state.left_trigger = std::max(0.0F, axis(4));
    state.right_trigger = std::max(0.0F, axis(5));
    const auto analog = [&](Control control, uint8_t mask, float x, float y = 0.0F) {
        if ((sample.supported_axes & mask) != 0)
            state.controls[static_cast<size_t>(control)] =
                !sample.available
                    ? ControlValue{ControlStatus::Unavailable}
                    : ControlValue{x != 0 || y != 0 ? ControlStatus::Active : ControlStatus::Released, x, y};
    };
    analog(Control::LeftStick, 0x03, state.left_x, state.left_y);
    analog(Control::RightStick, 0x0C, state.right_x, state.right_y);
    analog(Control::LeftTrigger, 0x10, state.left_trigger);
    analog(Control::RightTrigger, 0x20, state.right_trigger);
    const auto auxiliary = [&](Control control) -> std::optional<bool> {
        const auto value = state.controls[static_cast<size_t>(control)];
        if (value.status == ControlStatus::Unsupported || value.status == ControlStatus::Unavailable)
            return std::nullopt;
        return value.status == ControlStatus::Active;
    };
    state.home = auxiliary(Control::Guide);
    state.share = auxiliary(Control::Misc1);
    state.touchpad = auxiliary(Control::Touchpad);
    return state;
}

uint64_t SdlToCapture100ns(uint64_t ticks_ns, const SdlClockAnchor& anchor) noexcept {
    if (ticks_ns < anchor.ticks_ns) {
        const uint64_t delta = (anchor.ticks_ns - ticks_ns) / 100;
        return delta > anchor.capture_100ns ? 0 : anchor.capture_100ns - delta;
    }
    const uint64_t delta = (ticks_ns - anchor.ticks_ns) / 100;
    return delta > std::numeric_limits<uint64_t>::max() - anchor.capture_100ns ? std::numeric_limits<uint64_t>::max()
                                                                               : anchor.capture_100ns + delta;
}

std::optional<DeviceId> PersistentSdlDeviceId(uint16_t vendor, uint16_t product, std::string_view serial,
                                              std::string_view path) {
    if (serial.empty() || serial.size() > 32768 || path.size() > 32768)
        return std::nullopt;
    std::string discriminator(path);
    for (char& character : discriminator)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    if (!discriminator.starts_with("\\\\?\\"))
        return std::nullopt;
    discriminator.push_back('\0');
    discriminator.append(serial);
    const std::array<uint8_t, 6> prefix{2,
                                        1,
                                        static_cast<uint8_t>(vendor),
                                        static_cast<uint8_t>(vendor >> 8),
                                        static_cast<uint8_t>(product),
                                        static_cast<uint8_t>(product >> 8)};
    std::vector<uint8_t> data(prefix.begin(), prefix.end());
    data.insert(data.end(), discriminator.begin(), discriminator.end());
    DeviceId id{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), id.data(),
                   static_cast<ULONG>(id.size())) < 0)
        return std::nullopt;
    return id;
}

} // namespace exosnap::gamepad
