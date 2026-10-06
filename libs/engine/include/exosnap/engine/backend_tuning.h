#pragma once

#include "codec_types.h"

#include <cstdint>
#include <variant>

namespace exosnap::engine {

enum class NvencBRefMode { Off, Each, Middle };
enum class NvencMultipass { SinglePass, QuarterResolution, FullResolution };

// AV1 counts above seven require the hierarchical B-frame mode introduced after
// the pinned SDK. A driver capability alone does not make that mode executable.
[[nodiscard]] constexpr uint32_t MaxNvencBframes(VideoCodec codec, NvencBRefMode, uint32_t reported_max) noexcept {
    const uint32_t sdk_max = codec == VideoCodec::Av1 ? 7 : 31;
    return reported_max < sdk_max ? reported_max : sdk_max;
}

// The SDK bounds lookahead jointly with B-frames. The product also caps depth at 16 to bound surface memory before
// resolution-aware budgeting is available.
[[nodiscard]] constexpr uint32_t MaxNvencLookaheadDepth(uint32_t bframes) noexcept {
    const uint32_t sdk_max = bframes < 31 ? 31 - bframes : 0;
    return sdk_max < 16 ? sdk_max : 16;
}

// NVENC-specific tuning. P1-P7 is a speed/quality trade-off that only NVENC
// defines; it is not a universal encoder preset and must never be mapped onto
// another backend's nominally similar control.
struct NvencTuning {
    NvencPreset preset = NvencPreset::P4;
    uint32_t bframes = 0;
    NvencBRefMode b_ref_mode = NvencBRefMode::Off;
    bool lookahead = false;
    uint32_t lookahead_depth = 16;
    bool spatial_aq = false;
    bool temporal_aq = false;
    NvencMultipass multipass = NvencMultipass::SinglePass;

    bool operator==(const NvencTuning&) const noexcept = default;
};

// Backend-specific tuning selected for a recording. Exactly one alternative may
// be active; monostate means no backend-specific tuning is applied (a backend
// without a tuning type, or no backend at all). A future AMF or QSV backend
// adds its own alternative here without touching generic recording semantics.
using BackendTuning = std::variant<std::monostate, NvencTuning>;

// The NVENC tuning alternative, or nullptr when another backend (or none) is
// active. The encoder implementation and its Expert UI use this; generic code
// must not read an NVENC preset out of a tuning that a different backend owns.
[[nodiscard]] inline const NvencTuning* GetNvencTuning(const BackendTuning& tuning) noexcept {
    return std::get_if<NvencTuning>(&tuning);
}

} // namespace exosnap::engine
