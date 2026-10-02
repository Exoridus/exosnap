#pragma once

#include "adapter_capability.h"
#include "adapter_enum.h"
#include "capability_set.h"
#include "config_types.h"

#include <exosnap/engine/encoder_device.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace exosnap::capability {

// The format the recording wants, as the selector needs it. Only the facts a
// per-adapter probe actually answers are consulted: codec support and 4:4:4.
struct EncoderDeviceRequest {
    VideoCodec video_codec = VideoCodec::Av1;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    BitDepth bit_depth = BitDepth::Bit8;
};

// One enumerated adapter with its probed capability and the backend this build
// would use. Pure data: candidates are built from an existing scan, never by
// probing here.
struct EncoderDeviceCandidate {
    AdapterInfo adapter;
    AdapterEncoderCapability capability;
    exosnap::engine::EncoderBackend backend = exosnap::engine::EncoderBackend::None;
    bool supports_request = false;
    // Empty when this candidate can execute the request; otherwise the
    // structured reason the UI and session admission report.
    std::string unavailable_reason;

    // Usable = an implemented backend that probed successfully AND satisfies
    // the requested codec/chroma. Cross-adapter executability is a separate
    // resolver decision, not a property of the adapter alone.
    [[nodiscard]] bool usable() const noexcept {
        return backend != exosnap::engine::EncoderBackend::None && supports_request;
    }
};

// Builds one candidate per adapter. Every adapter is returned, including
// unusable ones, so the UI can show them disabled with a reason. Pairing is by
// position; the shorter input bounds the result.
std::vector<EncoderDeviceCandidate> BuildEncoderDeviceCandidates(std::span<const AdapterInfo> adapters,
                                                                 std::span<const AdapterEncoderCapability> capabilities,
                                                                 const EncoderDeviceRequest& request);

struct EncoderDeviceResolution {
    // A concrete, executable selection exists.
    bool resolved = false;
    // Auto only, when the capture adapter is not yet known: the session will
    // encode on the capture adapter at start. Never a guess.
    bool deferred_to_capture = false;
    // Index into the candidate span when >= 0.
    long candidate_index = -1;
    // Structured reason when not resolved.
    std::string reason;

    // The complete runtime role assignment the resolution describes. Capture
    // takes the adapter that owns the source; processing and encoder take the
    // selected device. Populated whenever a concrete device was identified,
    // INCLUDING a desired-but-not-executable split topology: the assignment is
    // never collapsed back to the capture adapter, so the model represents what
    // the user chose even while execution refuses it.
    exosnap::engine::PipelineAdapterAssignment assignment;
    // Validation of `assignment` against transport and backend facts. Only
    // authoritative when the capture adapter is known; with capture unknown an
    // Explicit resolution stays executable-at-start and is re-validated against
    // the real capture adapter by the engine session start.
    exosnap::engine::PipelineAssignmentValidation validation;
};

// Resolves a persisted preference against the current adapter scan.
//
// Explicit matches the fingerprint uniquely or fails; it never redirects to a
// different GPU, and an ambiguous match (two identical cards) is an error, not
// a coin toss. Auto selects the capture adapter when it is usable, because
// that is the only adapter this build can execute; when the capture adapter is
// unusable Auto fails honestly rather than choosing a GPU it cannot feed, and
// when the capture adapter is not yet known it defers to session start.
EncoderDeviceResolution ResolveEncoderDevice(std::span<const EncoderDeviceCandidate> candidates,
                                             const exosnap::engine::EncoderDevicePreference& preference,
                                             int64_t capture_adapter_luid, bool capture_adapter_known);

// The persistent fingerprint of an enumerated adapter. Pure.
[[nodiscard]] inline exosnap::engine::EncoderDeviceFingerprint
FingerprintFromAdapter(const AdapterInfo& adapter) noexcept {
    return {adapter.vendor_id, adapter.device_id, adapter.subsystem_id, adapter.name};
}

// A capability view scoped to one concrete adapter, derived from a system-wide
// set without touching hardware. NVIDIA codec/4:4:4 facts come from the
// adapter's own probe; a vendor with no implemented backend has every video
// path marked NotImplemented rather than inheriting another GPU's result.
CapabilitySet CapabilitySetForAdapter(const CapabilitySet& base, const AdapterInfo& adapter,
                                      const AdapterEncoderCapability& capability);

} // namespace exosnap::capability
