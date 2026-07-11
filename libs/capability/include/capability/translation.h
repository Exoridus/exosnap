#pragma once

#include "capability_set.h"
#include "resolver.h"
#include "runtime_snapshot.h"
#include "user_config.h"

#include <recorder_core/hdr_native.h>
#include <recorder_core/recorder_session.h>

namespace exosnap::capability {

recorder_core::RecorderConfig ToRecorderCoreConfig(const UserRecorderConfig& config, const CapabilitySet& caps,
                                                   ResolveResult* validation = nullptr);

// Translate the probed per-display HDR facts (DXGI_OUTPUT_DESC1 mirror) into
// the engine's HdrDisplayFacts for the native HDR10 encode path. One home for
// the field mapping so the session-start plumbing can never drift from the
// probe. sdr_white_level_nits stays at its 0 = unknown default: the capability
// probe does not read DISPLAYCONFIG_SDR_WHITE_LEVEL.
[[nodiscard]] inline recorder_core::HdrDisplayFacts ToHdrDisplayFacts(const DisplayHdrFacts& facts) noexcept {
    recorder_core::HdrDisplayFacts out;
    out.hdr_active = facts.hdr_active;
    out.red_primary_x = facts.red_primary_x;
    out.red_primary_y = facts.red_primary_y;
    out.green_primary_x = facts.green_primary_x;
    out.green_primary_y = facts.green_primary_y;
    out.blue_primary_x = facts.blue_primary_x;
    out.blue_primary_y = facts.blue_primary_y;
    out.white_point_x = facts.white_point_x;
    out.white_point_y = facts.white_point_y;
    out.max_luminance_nits = facts.max_luminance_nits;
    out.min_luminance_nits = facts.min_luminance_nits;
    return out;
}

} // namespace exosnap::capability
