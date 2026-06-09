#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace exosnap {

struct WebcamOverlayRect {
    float x_norm = 0.0f;
    float y_norm = 0.0f;
    float w_norm = 0.25f;
    float h_norm = 0.25f;

    static constexpr float kMinSizeNorm = 0.05f;
    static constexpr float kMaxSizeNorm = 1.0f;
};

// Canonical key-color modes for chroma keying.
enum class WebcamChromaKeyColorMode {
    Green,
    Blue,
    Magenta,
    Custom,
};

// Canonical preset key colors (linear sRGB 0–255).
inline constexpr uint8_t kChromaGreenR = 0, kChromaGreenG = 255, kChromaGreenB = 0;
inline constexpr uint8_t kChromaBlueR = 0, kChromaBlueG = 0, kChromaBlueB = 255;
inline constexpr uint8_t kChromaMagentaR = 255, kChromaMagentaG = 0, kChromaMagentaB = 255;

struct WebcamChromaKeySettings {
    bool enabled = false;

    // Selects which key color to use. When not Custom, active_color() returns the
    // canonical preset color; custom_r/g/b are only used for Custom mode.
    WebcamChromaKeyColorMode color_mode = WebcamChromaKeyColorMode::Green;

    // Custom key color — only meaningful when color_mode == Custom.
    uint8_t custom_r = 0;
    uint8_t custom_g = 255;
    uint8_t custom_b = 0;

    // All three parameters are normalized [0, 1]; 0–100 displayed to the user.
    float tolerance = 0.40f;       // keying radius in YCbCr chroma space
    float softness = 0.15f;        // gradient width beyond tolerance threshold
    float spill_reduction = 0.30f; // strength of key-chroma suppression on edges

    struct ActiveRgb {
        uint8_t r, g, b;
    };

    // Returns the effective key color based on color_mode.
    [[nodiscard]] ActiveRgb active_color() const noexcept {
        switch (color_mode) {
        case WebcamChromaKeyColorMode::Green:
            return {kChromaGreenR, kChromaGreenG, kChromaGreenB};
        case WebcamChromaKeyColorMode::Blue:
            return {kChromaBlueR, kChromaBlueG, kChromaBlueB};
        case WebcamChromaKeyColorMode::Magenta:
            return {kChromaMagentaR, kChromaMagentaG, kChromaMagentaB};
        case WebcamChromaKeyColorMode::Custom:
            return {custom_r, custom_g, custom_b};
        }
        return {kChromaGreenR, kChromaGreenG, kChromaGreenB};
    }

    bool operator==(const WebcamChromaKeySettings&) const = default;
};

struct WebcamSettings {
    bool enabled = false;
    std::string device_id;
    int width = 1280;
    int height = 720;
    int fps = 30;
    WebcamOverlayRect overlay;
    bool overlay_user_placed = false;
    bool aspect_ratio_locked = true;
    // Horizontal mirror of the webcam image. Default off. Applied identically to
    // the Settings preview, Record PiP and recorded output.
    bool mirror = false;
    WebcamChromaKeySettings chroma_key;
};

inline WebcamOverlayRect SanitizeWebcamOverlayRect(WebcamOverlayRect overlay) {
    auto clampFinite = [](float value, float min_value, float max_value, float fallback) {
        if (!std::isfinite(static_cast<double>(value))) {
            return fallback;
        }
        return std::clamp(value, min_value, max_value);
    };

    float x = clampFinite(overlay.x_norm, 0.0f, 1.0f - WebcamOverlayRect::kMinSizeNorm, 0.0f);
    float y = clampFinite(overlay.y_norm, 0.0f, 1.0f - WebcamOverlayRect::kMinSizeNorm, 0.0f);
    float w = clampFinite(overlay.w_norm, WebcamOverlayRect::kMinSizeNorm, WebcamOverlayRect::kMaxSizeNorm, 0.25f);
    float h = clampFinite(overlay.h_norm, WebcamOverlayRect::kMinSizeNorm, WebcamOverlayRect::kMaxSizeNorm, 0.25f);

    if (x + w > 1.0f) {
        w = (std::max)(WebcamOverlayRect::kMinSizeNorm, 1.0f - x);
    }
    if (y + h > 1.0f) {
        h = (std::max)(WebcamOverlayRect::kMinSizeNorm, 1.0f - y);
    }
    if (x + w > 1.0f) {
        x = (std::max)(0.0f, 1.0f - w);
    }
    if (y + h > 1.0f) {
        y = (std::max)(0.0f, 1.0f - h);
    }

    overlay.x_norm = x;
    overlay.y_norm = y;
    overlay.w_norm = w;
    overlay.h_norm = h;
    return overlay;
}

inline WebcamSettings SanitizeWebcamSettings(WebcamSettings settings) {
    settings.width = settings.width > 0 ? settings.width : 1280;
    settings.height = settings.height > 0 ? settings.height : 720;
    settings.fps = settings.fps > 0 ? settings.fps : 30;
    settings.overlay = SanitizeWebcamOverlayRect(settings.overlay);

    auto& ck = settings.chroma_key;
    // Enum: if out of range (e.g. from a cast), reset to Green.
    if (ck.color_mode != WebcamChromaKeyColorMode::Green && ck.color_mode != WebcamChromaKeyColorMode::Blue &&
        ck.color_mode != WebcamChromaKeyColorMode::Magenta && ck.color_mode != WebcamChromaKeyColorMode::Custom) {
        ck.color_mode = WebcamChromaKeyColorMode::Green;
    }
    if (!std::isfinite(static_cast<double>(ck.tolerance))) {
        ck.tolerance = 0.40f;
    }
    if (!std::isfinite(static_cast<double>(ck.softness))) {
        ck.softness = 0.15f;
    }
    if (!std::isfinite(static_cast<double>(ck.spill_reduction))) {
        ck.spill_reduction = 0.30f;
    }
    ck.tolerance = std::clamp(ck.tolerance, 0.0f, 1.0f);
    ck.softness = std::clamp(ck.softness, 0.0f, 1.0f);
    ck.spill_reduction = std::clamp(ck.spill_reduction, 0.0f, 1.0f);
    return settings;
}

} // namespace exosnap
