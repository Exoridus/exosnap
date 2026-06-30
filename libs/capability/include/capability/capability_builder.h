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

// Pure refinement: when a real per-GPU NVENC codec-GUID probe ran
// (facts.nvenc_codec_probed), downgrade any codec the GPU does NOT support to
// NotImplemented with a user-facing reason. When the probe did not run, the
// static baseline is left untouched (graceful degrade — never regress headless
// CI to "no codecs"). Called by BuildEffectiveCapabilities after the NVENC
// DLL-presence gate. Exposed for headless unit testing.
void ApplyNvencCodecSupport(CapabilitySet& caps, const NvidiaRuntimeFacts& facts);

} // namespace exosnap::capability
