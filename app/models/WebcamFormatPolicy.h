#pragma once

#include <algorithm>
#include <functional>
#include <vector>

namespace exosnap {

// One capture mode a camera offers: a native size at a rational frame rate.
struct WebcamFormat {
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
};

// Rounds a rational frame rate (fps_num/fps_den) to the nearest whole fps --
// never truncates. Common NTSC rates are the reason this matters: 30000/1001
// (~29.97 fps) must round to 30, not truncate to 29 (a plain fps_num/fps_den
// integer division). fps_den <= 0 is treated as 1. Shared by every place that
// turns a native MF frame rate into the plain-int fps WebcamFormat/
// WebcamSettings/the negotiated-fps report use, so the resolution combo's
// label, the stored setting, and the log field always agree with each other.
[[nodiscard]] inline int RoundWebcamFps(int fps_num, int fps_den) noexcept {
    const int den = (fps_den > 0) ? fps_den : 1;
    // Round-to-nearest via the standard (num + den/2) / den trick; fps_num is
    // always >= 0 in practice (an MF frame-rate ratio), so no sign handling.
    return (fps_num + den / 2) / den;
}

struct WebcamCaptureMode {
    int width = 0;
    int height = 0;
    int fps = 0;

    bool operator==(const WebcamCaptureMode&) const = default;
};

// Distinct whole frame rates offered at one size, highest first.
[[nodiscard]] inline std::vector<int> OfferedWebcamFrameRates(const std::vector<WebcamFormat>& offered, int width,
                                                              int height) {
    std::vector<int> rates;
    for (const WebcamFormat& format : offered) {
        if (format.width != width || format.height != height)
            continue;
        const int fps = RoundWebcamFps(format.fps_num, format.fps_den);
        if (fps > 0 && std::find(rates.begin(), rates.end(), fps) == rates.end())
            rates.push_back(fps);
    }
    std::sort(rates.begin(), rates.end(), std::greater<>());
    return rates;
}

// Distinct offered sizes, largest first.
[[nodiscard]] inline std::vector<WebcamCaptureMode> OfferedWebcamResolutions(const std::vector<WebcamFormat>& offered) {
    std::vector<WebcamCaptureMode> sizes;
    for (const WebcamFormat& format : offered) {
        const bool known = std::any_of(sizes.begin(), sizes.end(), [&](const WebcamCaptureMode& size) {
            return size.width == format.width && size.height == format.height;
        });
        if (!known)
            sizes.push_back({format.width, format.height, 0});
    }
    std::stable_sort(sizes.begin(), sizes.end(), [](const WebcamCaptureMode& a, const WebcamCaptureMode& b) {
        return static_cast<long long>(a.width) * a.height > static_cast<long long>(b.width) * b.height;
    });
    return sizes;
}

// The capture mode a request actually gets on a camera offering `offered`.
// The requested size wins when offered, else the smallest offered size with at
// least as many pixels (scaling a PiP down costs nothing visible, scaling it up
// does), else the largest offered size; then the offered rate closest to the
// requested one at that size (a tie goes to the higher one). With nothing
// offered (formats not known yet) the request is returned unchanged. Settings
// shows this mode and capture requests it, so the two cannot disagree about a
// resolution the camera does not have.
[[nodiscard]] inline WebcamCaptureMode ResolveWebcamCaptureMode(const std::vector<WebcamFormat>& offered, int width,
                                                                int height, int fps) {
    if (offered.empty())
        return {width, height, fps};
    const long long wanted_area = static_cast<long long>(width) * height;
    const auto area_of = [](const WebcamCaptureMode& size) { return static_cast<long long>(size.width) * size.height; };
    const std::vector<WebcamCaptureMode> sizes = OfferedWebcamResolutions(offered); // largest first
    WebcamCaptureMode mode = sizes.front();
    for (const WebcamCaptureMode& size : sizes) {
        if (size.width == width && size.height == height) {
            mode = size;
            break;
        }
        if (area_of(size) >= wanted_area)
            mode = size;
    }
    mode.fps = 0;
    int best_fps_distance = 0;
    for (const int candidate : OfferedWebcamFrameRates(offered, mode.width, mode.height)) {
        const int distance = std::abs(candidate - fps);
        if (mode.fps == 0 || distance < best_fps_distance) {
            mode.fps = candidate;
            best_fps_distance = distance;
        }
    }
    return mode;
}

} // namespace exosnap
