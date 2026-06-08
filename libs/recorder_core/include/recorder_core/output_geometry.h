#pragma once

#include <cstdint>
#include <optional>

namespace recorder_core {

enum class OutputFitMode {
    Contain,
};

struct FrameSize {
    uint32_t width = 0;
    uint32_t height = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return width > 0 && height > 0;
    }
};

struct ContentRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct OutputGeometry {
    FrameSize source;
    FrameSize output;
    ContentRect content;
};

[[nodiscard]] constexpr uint32_t AlignOutputDimensionEven(uint32_t value) noexcept {
    return value & ~1u;
}

[[nodiscard]] constexpr FrameSize AlignOutputSizeEven(FrameSize size) noexcept {
    return {AlignOutputDimensionEven(size.width), AlignOutputDimensionEven(size.height)};
}

[[nodiscard]] constexpr bool IsEncoderAlignedSize(FrameSize size) noexcept {
    return size.width >= 2 && size.height >= 2 && (size.width % 2u) == 0 && (size.height % 2u) == 0;
}

[[nodiscard]] inline std::optional<ContentRect> ResolveContainRect(FrameSize source, FrameSize output) noexcept {
    if (!source.IsValid() || !IsEncoderAlignedSize(output)) {
        return std::nullopt;
    }

    const uint64_t source_w = source.width;
    const uint64_t source_h = source.height;
    const uint64_t output_w = output.width;
    const uint64_t output_h = output.height;

    uint64_t scaled_w = output_w;
    uint64_t scaled_h = (source_h * output_w + source_w / 2u) / source_w;
    if (scaled_h == 0) {
        scaled_h = 1;
    }

    if (scaled_h > output_h) {
        scaled_h = output_h;
        scaled_w = (source_w * output_h + source_h / 2u) / source_h;
        if (scaled_w == 0) {
            scaled_w = 1;
        }
    }

    if (scaled_w > output_w) {
        scaled_w = output_w;
    }
    if (scaled_h > output_h) {
        scaled_h = output_h;
    }

    ContentRect rect;
    rect.width = static_cast<uint32_t>(scaled_w);
    rect.height = static_cast<uint32_t>(scaled_h);
    rect.x = static_cast<uint32_t>((output_w - scaled_w) / 2u);
    rect.y = static_cast<uint32_t>((output_h - scaled_h) / 2u);
    return rect;
}

[[nodiscard]] inline std::optional<OutputGeometry> ResolveOutputGeometry(FrameSize source, FrameSize output) noexcept {
    const std::optional<ContentRect> content = ResolveContainRect(source, output);
    if (!content.has_value()) {
        return std::nullopt;
    }
    return OutputGeometry{source, output, *content};
}

} // namespace recorder_core
