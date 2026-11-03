#pragma once

#include <exosnap/engine/color_metadata.h>
#include <exosnap/engine/visual_scene.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace exosnap::engine {

// Camera payload layouts, all tightly packed (no row padding). The YUV layouts
// reach the GPU unconverted and are converted there once per generation.
enum class WebcamPixelFormat : uint8_t {
    Bgra,
    Nv12,
    Yuy2,
};

// Returns zero for invalid dimensions or unsupported format alignment.
[[nodiscard]] inline size_t WebcamFramePayloadBytes(WebcamPixelFormat format, int width, int height) noexcept {
    if (width <= 0 || height <= 0)
        return 0;
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    switch (format) {
    case WebcamPixelFormat::Bgra:
        return w * h * 4u;
    case WebcamPixelFormat::Nv12:
        return (width % 2 == 0 && height % 2 == 0) ? w * h * 3u / 2u : 0;
    case WebcamPixelFormat::Yuy2:
        return width % 2 == 0 ? w * h * 2u : 0;
    }
    return 0;
}

struct WebcamFrameSnapshot {
    int width = 0;
    int height = 0;
    uint64_t generation = 0;
    std::vector<uint8_t> pixels;
    WebcamPixelFormat format = WebcamPixelFormat::Bgra;
    // YUV defaults match UVC media types that omit matrix/range metadata.
    MatrixCoefficients matrix = MatrixCoefficients::Bt601;
    ColorRange range = ColorRange::Limited;

    [[nodiscard]] bool HasCompletePayload() const noexcept {
        const size_t required = WebcamFramePayloadBytes(format, width, height);
        return required != 0 && pixels.size() >= required;
    }
};

struct CpuImageLayout {
    int width = 0;
    int height = 0;
    WebcamPixelFormat format = WebcamPixelFormat::Bgra;
    MatrixCoefficients matrix = MatrixCoefficients::Bt601;
    ColorRange range = ColorRange::Limited;
    auto operator<=>(const CpuImageLayout&) const = default;
};

struct VisualFrameStamp {
    SourceToken token;
    uint64_t sequence = 0;
    CaptureTime100ns received_at;
    std::optional<CaptureTime100ns> captured_at;
};

struct VisualBudgetPolicy {
    uint32_t camera_slots = 3;
    uint32_t max_resource_generations = 2;
    std::optional<uint64_t> cpu_payload_limit_bytes;
};

enum class PublishResult { Accepted, Superseded, Revoked, NoFreeSlot, Invalid };
enum class ReadResult { Updated, Unchanged, NotReady, Busy, Revoked };
enum class HoldPolicy { KeepAccepted, ClearAccepted };

} // namespace exosnap::engine
