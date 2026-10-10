#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace exosnap {

// The standard UVC camera controls (USB Video Class processing unit and camera
// terminal) a webcam may expose. A camera reports which of them it supports and
// their ranges; nothing here assumes any one is present.
enum class WebcamControl : uint8_t {
    Brightness,
    Contrast,
    Hue,
    Saturation,
    Sharpness,
    Gamma,
    WhiteBalance,
    BacklightCompensation,
    Gain,
    PowerLineFrequency,
    Exposure,
    Focus,
    Zoom,
    Pan,
    Tilt,
};

inline constexpr std::array<WebcamControl, 15> kAllWebcamControls = {
    WebcamControl::Brightness,   WebcamControl::Contrast,
    WebcamControl::Hue,          WebcamControl::Saturation,
    WebcamControl::Sharpness,    WebcamControl::Gamma,
    WebcamControl::WhiteBalance, WebcamControl::BacklightCompensation,
    WebcamControl::Gain,         WebcamControl::PowerLineFrequency,
    WebcamControl::Exposure,     WebcamControl::Focus,
    WebcamControl::Zoom,         WebcamControl::Pan,
    WebcamControl::Tilt,
};

// Values of WebcamControl::PowerLineFrequency, as UVC defines them.
enum class WebcamPowerLineFrequency : long {
    Disabled = 0,
    Hz50 = 1,
    Hz60 = 2,
    Auto = 3,
};

// Stable persistence key.
[[nodiscard]] constexpr std::string_view WebcamControlKey(WebcamControl control) noexcept {
    switch (control) {
    case WebcamControl::Brightness:
        return "brightness";
    case WebcamControl::Contrast:
        return "contrast";
    case WebcamControl::Hue:
        return "hue";
    case WebcamControl::Saturation:
        return "saturation";
    case WebcamControl::Sharpness:
        return "sharpness";
    case WebcamControl::Gamma:
        return "gamma";
    case WebcamControl::WhiteBalance:
        return "white_balance";
    case WebcamControl::BacklightCompensation:
        return "backlight_compensation";
    case WebcamControl::Gain:
        return "gain";
    case WebcamControl::PowerLineFrequency:
        return "power_line_frequency";
    case WebcamControl::Exposure:
        return "exposure";
    case WebcamControl::Focus:
        return "focus";
    case WebcamControl::Zoom:
        return "zoom";
    case WebcamControl::Pan:
        return "pan";
    case WebcamControl::Tilt:
        return "tilt";
    }
    return {};
}

[[nodiscard]] inline std::optional<WebcamControl> WebcamControlFromKey(std::string_view key) noexcept {
    for (const WebcamControl control : kAllWebcamControls) {
        if (WebcamControlKey(control) == key)
            return control;
    }
    return std::nullopt;
}

// What one camera reports for one control.
struct WebcamControlRange {
    WebcamControl control = WebcamControl::Brightness;
    long minimum = 0;
    long maximum = 0;
    long step = 1;
    long default_value = 0;
    // The camera's state when it was probed, before this application set it.
    long current = 0;
    bool current_automatic = false;
    bool can_automatic = false;
    bool can_manual = true;
};

// A control the user set. A control without one stays as the camera has it.
struct WebcamControlSetting {
    WebcamControl control = WebcamControl::Brightness;
    bool automatic = false;
    long value = 0;

    bool operator==(const WebcamControlSetting&) const = default;
};

// The value a camera can actually take: inside its range and on its step grid.
[[nodiscard]] inline long ClampWebcamControlValue(const WebcamControlRange& range, long value) noexcept {
    if (range.maximum < range.minimum)
        return range.minimum;
    long clamped = std::clamp(value, range.minimum, range.maximum);
    if (range.step > 1) {
        const long offset = clamped - range.minimum;
        const long snapped = range.minimum + ((offset + range.step / 2) / range.step) * range.step;
        clamped = std::min(snapped, range.maximum);
    }
    return clamped;
}

// One setting per control, in enum order, so equal configurations compare equal.
[[nodiscard]] inline std::vector<WebcamControlSetting>
NormalizeWebcamControlSettings(std::vector<WebcamControlSetting> settings) {
    std::stable_sort(settings.begin(), settings.end(), [](const auto& a, const auto& b) {
        return static_cast<int>(a.control) < static_cast<int>(b.control);
    });
    // The later of two settings for one control wins, as it would in an edit.
    std::vector<WebcamControlSetting> unique;
    for (const WebcamControlSetting& setting : settings) {
        if (!unique.empty() && unique.back().control == setting.control)
            unique.back() = setting;
        else
            unique.push_back(setting);
    }
    return unique;
}

[[nodiscard]] inline const WebcamControlSetting*
FindWebcamControlSetting(const std::vector<WebcamControlSetting>& settings, WebcamControl control) noexcept {
    for (const WebcamControlSetting& setting : settings) {
        if (setting.control == control)
            return &setting;
    }
    return nullptr;
}

} // namespace exosnap
