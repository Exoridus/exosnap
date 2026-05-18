#pragma once

#include "capability_set.h"

namespace exosnap::capability {

class CapabilityBuilder {
public:
    static CapabilitySet BuildStaticValidatedBaseline();
    static CapabilitySet BuildFromHardwareQuery();
};

} // namespace exosnap::capability

