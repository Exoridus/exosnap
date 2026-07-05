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

// Pure refinement: when a real per-GPU NVENC probe ran (facts.nvenc_codec_probed),
// downgrade per-codec 4:4:4 (chroma444) to NotImplemented for any codec whose
// NV_ENC_CAPS_SUPPORT_YUV444_ENCODE probe was false. AV1 is always NotImplemented
// (no NVENC 4:4:4). When the probe did not run, the ValidUnvalidated baseline is
// left untouched. Called by BuildEffectiveCapabilities. Exposed for unit testing.
void ApplyNvencYuv444Support(CapabilitySet& caps, const NvidiaRuntimeFacts& facts);

} // namespace exosnap::capability
