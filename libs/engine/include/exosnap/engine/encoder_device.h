#pragma once

#include <cstdint>
#include <string>

namespace exosnap::engine {

// Encoder backends this build implements. A backend is an implementation fact,
// not a hardware fact: an AMD or Intel adapter is a recordable device the
// moment a matching backend exists, and None means "no backend in this build",
// never "not a GPU".
enum class EncoderBackend { None, Nvenc };

// The backend this build would use for a PCI vendor ID (0x10DE = NVIDIA).
// Takes the raw ID, not capability::AdapterVendor, so this public header stays
// free of the capability library and its include path reaches every consumer
// of RecorderConfig. Pure.
[[nodiscard]] inline constexpr EncoderBackend ImplementedEncoderBackendForVendorId(uint32_t vendor_id) noexcept {
    return vendor_id == 0x10DEu ? EncoderBackend::Nvenc : EncoderBackend::None;
}

// Best-effort persistent identity of a physical adapter. The DXGI LUID is
// boot-scoped (see capability::AdapterInfo::luid) and is deliberately absent
// here: it may be used to re-locate an adapter within one boot, never as a
// value written to disk. vendor/device/subsystem are the PCI facts DXGI
// reports; the name is display/re-resolution context, not part of the match.
struct EncoderDeviceFingerprint {
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t subsystem_id = 0;
    std::string name;

    bool operator==(const EncoderDeviceFingerprint&) const noexcept = default;
};

// True when two fingerprints name the same physical hardware model. The display
// name is intentionally ignored: a driver can rename an adapter, and two
// identical cards share every PCI fact, which is exactly the ambiguity callers
// must detect rather than guess through.
[[nodiscard]] inline bool SameHardwareIdentity(const EncoderDeviceFingerprint& a,
                                               const EncoderDeviceFingerprint& b) noexcept {
    return a.vendor_id == b.vendor_id && a.device_id == b.device_id && a.subsystem_id == b.subsystem_id;
}

// What the user chose, as persisted. The choice names the preferred device for
// the whole post-capture processing + encoding path, not a fixed-function
// encoder block: composition, tone mapping, scaling and color conversion run on
// the same adapter as the encoder in the executable topology. Auto carries no
// device and is resolved at session start; Explicit names one physical device by
// fingerprint and is never silently redirected to a different GPU.
struct EncoderDevicePreference {
    enum class Mode { Auto, Explicit };
    Mode mode = Mode::Auto;
    EncoderDeviceFingerprint device;

    bool operator==(const EncoderDevicePreference&) const noexcept = default;
};

// ── Pipeline adapter roles ──────────────────────────────────────────────────────

// The three physical-adapter roles the pipeline distinguishes. A role is a
// pipeline fact, never inferred from the vendor: one adapter may serve any
// combination, and the model must survive them being split later. The 0.10
// build executes only the all-on-one-adapter topology.
enum class PipelineRole : uint8_t {
    None = 0,
    Capture = 1,
    Processing = 2,
    Encoder = 4,
};

[[nodiscard]] constexpr PipelineRole operator|(PipelineRole a, PipelineRole b) noexcept {
    return static_cast<PipelineRole>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr bool HasRole(PipelineRole mask, PipelineRole role) noexcept {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(role)) != 0;
}

// One physical adapter as a pipeline role sees it. `luid` is the boot-scoped
// packed DXGI identity (capability::PackAdapterLuid); `device` is the persistent
// fingerprint; `backend` is the implemented encoding backend for the vendor.
// Identity, role and capability stay separate: none is derived from another.
struct PipelineAdapterIdentity {
    bool known = false;
    int64_t luid = 0;
    uint32_t vendor_id = 0;
    EncoderBackend backend = EncoderBackend::None;
    EncoderDeviceFingerprint device;
};

// The complete runtime assignment of physical adapters to pipeline roles.
// Runtime only, never persisted: the capture role derives from the actual
// capture source, and processing/encoder from the resolved device preference.
// Processing and encoder are separate fields even while production requires
// them to be the same adapter.
struct PipelineAdapterAssignment {
    PipelineAdapterIdentity capture;
    PipelineAdapterIdentity processing;
    PipelineAdapterIdentity encoder;
};

// Whether a captured surface can reach the processing adapter. This build has
// no transfer path, so two different physical adapters are unsupported.
// CrossAdapterShared names a real future transport; keeping it in the policy
// lets validation be exercised for a split topology before transport exists.
enum class CaptureSurfaceTransport {
    Unknown,
    SameAdapter,
    CrossAdapterUnsupported,
    CrossAdapterShared,
};

// The one place adapter comparison is translated into a transport verdict.
[[nodiscard]] inline CaptureSurfaceTransport
CaptureSurfaceTransportFor(const PipelineAdapterIdentity& capture, const PipelineAdapterIdentity& processing) noexcept {
    if (!capture.known || !processing.known) {
        return CaptureSurfaceTransport::Unknown;
    }
    return capture.luid == processing.luid ? CaptureSurfaceTransport::SameAdapter
                                           : CaptureSurfaceTransport::CrossAdapterUnsupported;
}

// Executability facts for a candidate assignment. Production derives them from
// the resolver's candidate probe; tests may supply synthetic backend facts so
// the pure validation policy can accept a topology this build does not run.
struct PipelineAssignmentFacts {
    PipelineAdapterAssignment assignment;
    bool processing_executable = false;
    std::string processing_reason;
    bool encoder_executable = false;
    std::string encoder_reason;
    CaptureSurfaceTransport transport = CaptureSurfaceTransport::Unknown;
};

struct PipelineAssignmentValidation {
    bool ok = false;
    std::string reason;
};

// Validates a complete role assignment against transport and backend facts.
// Pure. The transport seam is consulted instead of a second adapter equality
// check, so a future shared-surface transport changes only
// CaptureSurfaceTransportFor.
[[nodiscard]] inline PipelineAssignmentValidation ValidatePipelineAssignment(const PipelineAssignmentFacts& facts) {
    if (!facts.assignment.capture.known) {
        return {false, "The capture adapter is not identified yet."};
    }
    if (!facts.assignment.processing.known) {
        return {false, "The selected processing device could not be resolved."};
    }
    if (!facts.assignment.encoder.known) {
        return {false, "The selected encoder device could not be resolved."};
    }
    if (facts.transport != CaptureSurfaceTransport::SameAdapter &&
        facts.transport != CaptureSurfaceTransport::CrossAdapterShared) {
        return {false, "The selected processing device is on a different adapter than the capture source; "
                       "cross-adapter transport is not implemented."};
    }
    if (!facts.processing_executable) {
        return {false, facts.processing_reason.empty() ? "The selected processing device cannot run the pipeline."
                                                       : facts.processing_reason};
    }
    if (!facts.encoder_executable) {
        return {false, facts.encoder_reason.empty() ? "The selected encoder device cannot execute the requested format."
                                                    : facts.encoder_reason};
    }
    return {true, {}};
}

// Runtime resolution of a preference against the adapters present in this boot.
// The resolved device serves the processing and encoding roles, which share an
// adapter in the executable topology. valid == false is a first-class answer:
// the reason carries the structured explanation, and callers must fail or fall
// back to Auto rather than guess.
struct ResolvedEncoderDevice {
    bool valid = false;
    int64_t adapter_luid = 0; // boot-scoped, from capability::PackAdapterLuid
    uint32_t vendor_id = 0;
    EncoderBackend backend = EncoderBackend::None;
    EncoderDeviceFingerprint device;
    std::string reason;
};

// Verifies the app-resolved encoder device against the adapter the capture D3D
// device actually landed on. Pure, so the engine's session-start decision is
// testable without hardware.
//
// An unresolved Explicit preference fails closed; only Auto may proceed without
// a resolution (it encodes on the capture adapter). A resolved device on a
// different adapter fails: the surfaces belong to the capture device, and
// cross-adapter transport does not exist. Auto resolves to the capture adapter,
// so this can only fire for a stale or hand-built explicit resolution.
struct EncoderDeviceValidation {
    bool ok = false;
    std::string reason;
};

[[nodiscard]] inline EncoderDeviceValidation ValidateEncoderDeviceForCapture(const EncoderDevicePreference& preference,
                                                                             const ResolvedEncoderDevice& resolved,
                                                                             int64_t capture_adapter_luid) {
    if (preference.mode == EncoderDevicePreference::Mode::Explicit && !resolved.valid) {
        return {false, resolved.reason.empty() ? "the selected encoder device could not be resolved" : resolved.reason};
    }
    if (resolved.valid) {
        PipelineAdapterIdentity capture;
        capture.known = true;
        capture.luid = capture_adapter_luid;
        PipelineAdapterIdentity processing;
        processing.known = true;
        processing.luid = resolved.adapter_luid;
        if (CaptureSurfaceTransportFor(capture, processing) != CaptureSurfaceTransport::SameAdapter) {
            return {false, "the selected encoder device is on a different adapter than the capture source; "
                           "cross-adapter encoding is not implemented"};
        }
    }
    return {true, {}};
}

} // namespace exosnap::engine
