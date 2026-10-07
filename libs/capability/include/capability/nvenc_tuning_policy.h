#pragma once

#include "capability_set.h"

#include <exosnap/engine/backend_tuning.h>

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::capability {

enum class NvencLookaheadPolicy { Explicit, Auto };

struct NvencTuningResolution {
    engine::NvencTuning tuning;
    bool available = false;
    int max_bframes = 0;
    std::vector<engine::NvencBRefMode> b_ref_modes{engine::NvencBRefMode::Off};
    bool lookahead_supported = false;
    bool lookahead_auto_enabled = false;
    uint32_t lookahead_min_depth = 1;
    uint32_t lookahead_max_depth = 0;
    bool spatial_aq_supported = false;
    bool temporal_aq_supported = false;
    bool multipass_supported = false;
    std::string reason;
    std::string bframes_reason;
    std::string b_ref_reason;
    std::string lookahead_reason;
    std::string spatial_aq_reason;
    std::string temporal_aq_reason;
    std::string multipass_reason;
};

// Resolves stored NVENC preferences for the probed adapter without changing
// those preferences. Unavailable features are pinned to conservative values.
// Auto enables Lookahead 16 for supported NVENC AV1/VBR. Other combinations
// resolve Off without changing the explicit wish or depth.
NvencTuningResolution ResolveNvencTuning(const engine::NvencTuning& requested, const CapabilitySet& caps,
                                         VideoCodec codec, engine::RateControlMode rate_control,
                                         NvencLookaheadPolicy policy = NvencLookaheadPolicy::Explicit);

} // namespace exosnap::capability
