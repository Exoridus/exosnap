#pragma once
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
    bool aspect_ratio_locked = true;
    WebcamChromaKeySettings chroma_key;
};

} // namespace exosnap
