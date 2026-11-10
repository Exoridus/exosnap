#pragma once

#include "gamepad/gamepad.h"
#include <SDL3/SDL_gamepad.h>

namespace exosnap::gamepad {

[[nodiscard]] LayoutKind SdlLayoutKindFor(SDL_GamepadType type, uint16_t vendor, uint16_t product) noexcept;

struct SdlGamepadSample {
    std::array<int16_t, 6> axes{};
    uint32_t buttons = 0;
    uint32_t supported_buttons = 0;
    uint8_t supported_axes = 0;
    bool available = true;
};

[[nodiscard]] GamepadState NormalizeSdlControls(const SdlGamepadSample& sample) noexcept;

struct SdlClockAnchor {
    uint64_t ticks_ns = 0;
    uint64_t capture_100ns = 0;
};

[[nodiscard]] uint64_t SdlToCapture100ns(uint64_t ticks_ns, const SdlClockAnchor& anchor) noexcept;

/// Persistence requires both a reported serial and a native interface path.
/// Changing either requires reselection. Serial-only and slot identities are ambiguous.
[[nodiscard]] std::optional<DeviceId> PersistentSdlDeviceId(uint16_t vendor, uint16_t product, std::string_view serial,
                                                            std::string_view path);

/// Test seam for an open failure SDL cannot produce on demand. While set, an instance for
/// which the predicate returns true is handled as a failed `SDL_OpenGamepad`. Production
/// code never sets it. The predicate runs on the input worker thread.
using SdlOpenFailureForTest = bool (*)(SDL_JoystickID instance);
void SetSdlOpenFailureForTest(SdlOpenFailureForTest predicate) noexcept;

} // namespace exosnap::gamepad
