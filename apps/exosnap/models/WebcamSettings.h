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

struct WebcamChromaKeySettings {
    bool enabled = false;
    uint8_t r = 0;
    uint8_t g = 177;
    uint8_t b = 64;
    float tolerance = 0.30f;
    float softness = 0.05f;
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

    if (!std::isfinite(static_cast<double>(settings.chroma_key.tolerance))) {
        settings.chroma_key.tolerance = 0.30f;
    }
    if (!std::isfinite(static_cast<double>(settings.chroma_key.softness))) {
        settings.chroma_key.softness = 0.05f;
    }
    settings.chroma_key.tolerance = std::clamp(settings.chroma_key.tolerance, 0.0f, 1.0f);
    settings.chroma_key.softness = std::clamp(settings.chroma_key.softness, 0.0f, 1.0f);
    return settings;
}

} // namespace exosnap
