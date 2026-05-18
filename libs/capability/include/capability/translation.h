#pragma once

#include "capability_set.h"
#include "resolver.h"
#include "user_config.h"

#include <recorder_core/recorder_session.h>

namespace exosnap::capability {

recorder_core::RecorderConfig ToRecorderCoreConfig(
    const UserRecorderConfig& config,
    const CapabilitySet& caps,
    ResolveResult* validation = nullptr);

} // namespace exosnap::capability

