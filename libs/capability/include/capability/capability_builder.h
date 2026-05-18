#pragma once

#include "capability_set.h"
#include "runtime_snapshot.h"

namespace exosnap::capability {

class CapabilityBuilder {
  public:
    static CapabilitySet BuildStaticValidatedBaseline();

    static RuntimeCapabilitySnapshot QueryRuntimeFacts();

    static CapabilitySet BuildEffectiveCapabilities(const RuntimeCapabilitySnapshot& snapshot);

    static CapabilitySet BuildFromHardwareQuery();
};

} // namespace exosnap::capability
