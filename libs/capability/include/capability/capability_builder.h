#pragma once

#include "capability_set.h"
#include "runtime_snapshot.h"

namespace exosnap::capability {

class CapabilityBuilder {
  public:
    static CapabilitySet BuildStaticValidatedBaseline();

    static RuntimeCapabilitySnapshot QueryRuntimeFacts();

    // Just the per-display facts (HDR colour space, bit depth, primaries, luminance).
    //
    // QueryRuntimeFacts() runs once at startup and its display facts go stale as soon as
    // the user toggles Windows HDR or Advanced Color — neither changes screen geometry,
    // so nothing notices. This is the cheap re-read: DXGI enumeration plus GetDesc1, with
    // none of the expensive probes (no NVENC session, no Media Foundation).
    static std::vector<DisplayHdrFacts> QueryDisplayFacts();

    // Cheap adapter-identity read (LUID + WDDM driver version) — see
    // AdapterIdentity's doc comment in runtime_snapshot.h. Used to build the
    // disk-cache key for the warm-start path; never consulted by the real
    // probe itself.
    static AdapterIdentity QueryAdapterIdentity();

    // Pure: derives a CapabilitySet from an already-obtained snapshot, without
    // touching hardware. Leaves CapabilitySet::probed at its default (false) —
    // callers that pass a snapshot obtained from a real probe just now
    // (BuildFromHardwareQuery, below) are responsible for marking it probed;
    // callers rebuilding from a disk-cache snapshot must NOT.
    static CapabilitySet BuildEffectiveCapabilities(const RuntimeCapabilitySnapshot& snapshot);

    // Runs the real, synchronous hardware probe (QueryRuntimeFacts) and derives
    // the effective CapabilitySet from it, with probed = true. The only path
    // that may ever unlock a recording-start decision (see CapabilitySet::probed).
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
