#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace exosnap::engine {

struct CompositionSize {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const CompositionSize&) const = default;
};

// Unknown geometry must preserve source detail. Hidden consumers request no pixels.
[[nodiscard]] inline std::optional<CompositionSize> ResolvePreviewPixelDemand(double width, double height,
                                                                              double device_pixel_ratio,
                                                                              double crop_width, double crop_height,
                                                                              bool visible) noexcept {
    if (!visible)
        return CompositionSize{};
    if (!(crop_width > 0 && crop_width <= 1 && crop_height > 0 && crop_height <= 1) ||
        !std::isfinite(device_pixel_ratio) || device_pixel_ratio <= 0)
        return std::nullopt;
    const double pixel_width = width * device_pixel_ratio / crop_width;
    const double pixel_height = height * device_pixel_ratio / crop_height;
    if (!std::isfinite(pixel_width) || !std::isfinite(pixel_height) || pixel_width <= 0 || pixel_height <= 0 ||
        pixel_width >= UINT32_MAX || pixel_height >= UINT32_MAX)
        return std::nullopt;
    return CompositionSize{static_cast<uint32_t>(std::ceil(pixel_width)),
                           static_cast<uint32_t>(std::ceil(pixel_height))};
}
enum class CompositionColor : uint8_t { Sdr, Hdr, Mixed };

struct RenderDemand {
    uint64_t consumer = 0;
    uint64_t scene = 0;
    uint64_t revision = 0;
    CompositionSize pixels;
    uint32_t cadence = 0;
    CompositionColor color = CompositionColor::Sdr;
    bool alpha = false;
    bool visible = true;
    bool full_scene = true;
    uint32_t cadence_denominator = 1;
};

struct CompositionDemandPlan {
    CompositionSize pixels;
    bool reduced = false;
};

// Pixel requests include physical viewport scale and zoom. Only identical full-scene
// geometry and SDR representations share a reduced target; all others retain the oracle.
[[nodiscard]] constexpr CompositionDemandPlan
ResolveCompositionDemand(CompositionSize source, std::span<const RenderDemand> demands, bool enabled) noexcept {
    CompositionDemandPlan fallback{source, false};
    if (!enabled || source.width == 0 || source.height == 0)
        return fallback;
    const RenderDemand* first = nullptr;
    CompositionSize selected{};
    for (const auto& demand : demands) {
        if (!demand.visible)
            continue;
        if (demand.consumer == 0 || demand.scene == 0 || demand.cadence == 0 || demand.cadence_denominator == 0 ||
            !demand.full_scene || demand.color != CompositionColor::Sdr || demand.alpha || demand.pixels.width == 0 ||
            demand.pixels.height == 0 || demand.pixels.width > source.width || demand.pixels.height > source.height ||
            uint64_t{demand.pixels.width} * source.height != uint64_t{demand.pixels.height} * source.width)
            return fallback;
        if (first && (demand.scene != first->scene || demand.revision != first->revision ||
                      uint64_t{demand.cadence} * first->cadence_denominator !=
                          uint64_t{first->cadence} * demand.cadence_denominator))
            return fallback;
        if (!first)
            first = &demand;
        if (demand.pixels.width > selected.width)
            selected = demand.pixels;
    }
    if (!first)
        return fallback;
    return {selected, selected != source};
}

} // namespace exosnap::engine
