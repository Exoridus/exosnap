#pragma once

#include "codec_types.h"

#include <variant>

namespace exosnap::engine {

// NVENC-specific tuning. P1-P7 is a speed/quality trade-off that only NVENC
// defines; it is not a universal encoder preset and must never be mapped onto
// another backend's nominally similar control.
struct NvencTuning {
    NvencPreset preset = NvencPreset::P4;

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
