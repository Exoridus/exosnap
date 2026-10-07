#include <capability/encoder_device_resolver.h>
#include <capability/translatable.h>

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/runtime_snapshot.h>
#include <capability/support_level.h>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/encoder_device.h>
#include <span>
#include <utility>
#include <vector>

#include <capability/capability_builder.h>

namespace exosnap::capability {
namespace {

bool CodecSupported(const AdapterEncoderCapability& capability, VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::Av1:
        return capability.av1;
    case VideoCodec::Hevc:
        return capability.hevc;
    case VideoCodec::H264:
        return capability.h264;
    }
    return false;
}

bool ChromaSupported(const AdapterEncoderCapability& capability, VideoCodec codec, ChromaSubsampling chroma) noexcept {
    switch (chroma) {
    case ChromaSubsampling::Cs420:
        return true;
    case ChromaSubsampling::Cs444:
        if (codec == VideoCodec::H264) {
            return capability.yuv444_h264;
        }
        if (codec == VideoCodec::Hevc) {
            return capability.yuv444_hevc;
        }
        return false; // AV1 has no 4:4:4 path
    case ChromaSubsampling::Cs422:
        return false; // not implemented for any backend
    }
    return false;
}

exosnap::engine::PipelineAdapterIdentity IdentityFromCandidate(const EncoderDeviceCandidate& candidate) {
    exosnap::engine::PipelineAdapterIdentity identity;
    identity.known = true;
    identity.luid = candidate.adapter.luid;
    identity.vendor_id = candidate.adapter.vendor_id;
    identity.backend = candidate.backend;
    identity.device = FingerprintFromAdapter(candidate.adapter);
    return identity;
}

const EncoderDeviceCandidate* FindCandidateByLuid(std::span<const EncoderDeviceCandidate> candidates,
                                                  int64_t luid) noexcept {
    if (luid == 0) {
        return nullptr;
    }
    for (const auto& candidate : candidates) {
        if (candidate.adapter.luid == luid) {
            // candidate refers to an element in caller-owned storage; the span does not own it.
            // cppcheck-suppress returnDanglingLifetime
            return &candidate;
        }
    }
    return nullptr;
}

// Fills the role assignment and validates it against the selected candidate's
// facts. `capture_adapter_known` false leaves the capture role unknown; the
// caller decides what that means for executability at the call site.
exosnap::engine::PipelineAssignmentValidation PopulateAssignment(EncoderDeviceResolution& result,
                                                                 const EncoderDeviceCandidate& selected,
                                                                 std::span<const EncoderDeviceCandidate> candidates,
                                                                 int64_t capture_adapter_luid,
                                                                 bool capture_adapter_known) {
    result.assignment.processing = IdentityFromCandidate(selected);
    result.assignment.encoder = IdentityFromCandidate(selected);
    if (capture_adapter_known) {
        if (const EncoderDeviceCandidate* capture = FindCandidateByLuid(candidates, capture_adapter_luid)) {
            result.assignment.capture = IdentityFromCandidate(*capture);
        } else {
            result.assignment.capture.known = true;
            result.assignment.capture.luid = capture_adapter_luid;
        }
    }

    exosnap::engine::PipelineAssignmentFacts facts;
    facts.assignment = result.assignment;
    facts.transport =
        exosnap::engine::CaptureSurfaceTransportFor(result.assignment.capture, result.assignment.processing);
    facts.processing_executable = selected.usable();
    facts.processing_reason = selected.unavailable_reason;
    facts.encoder_executable = selected.usable();
    facts.encoder_reason = selected.unavailable_reason;
    return exosnap::engine::ValidatePipelineAssignment(facts);
}

std::string UnavailableReason(const EncoderDeviceCandidate& candidate, const EncoderDeviceRequest& request) {
    if (candidate.backend == exosnap::engine::EncoderBackend::None) {
        return EXOSNAP_TRANSLATABLE("Capabilities",
                                    "No encoder backend is implemented for this adapter in this build.");
    }
    if (!candidate.capability.probed) {
        return candidate.capability.provenance.empty()
                   ? EXOSNAP_TRANSLATABLE("Capabilities",
                                          "The encoder capability probe did not complete on this adapter.")
                   : candidate.capability.provenance;
    }
    if (!CodecSupported(candidate.capability, request.video_codec)) {
        return std::string(ToString(request.video_codec)) + " is not supported by this encoder on this adapter.";
    }
    if (request.chroma == ChromaSubsampling::Cs444) {
        return "4:4:4 is not supported for " + std::string(ToString(request.video_codec)) + " on this adapter.";
    }
    if (request.chroma == ChromaSubsampling::Cs422) {
        return "4:2:2 chroma is not implemented.";
    }
    return EXOSNAP_TRANSLATABLE("Capabilities", "This adapter cannot execute the requested format.");
}

} // namespace

std::vector<EncoderDeviceCandidate> BuildEncoderDeviceCandidates(std::span<const AdapterInfo> adapters,
                                                                 std::span<const AdapterEncoderCapability> capabilities,
                                                                 const EncoderDeviceRequest& request) {
    const size_t count = adapters.size() < capabilities.size() ? adapters.size() : capabilities.size();
    std::vector<EncoderDeviceCandidate> candidates;
    candidates.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        EncoderDeviceCandidate candidate;
        candidate.adapter = adapters[i];
        candidate.capability = capabilities[i];
        candidate.backend = exosnap::engine::ImplementedEncoderBackendForVendorId(candidate.adapter.vendor_id);
        candidate.supports_request = candidate.backend != exosnap::engine::EncoderBackend::None &&
                                     candidate.capability.probed &&
                                     CodecSupported(candidate.capability, request.video_codec) &&
                                     ChromaSupported(candidate.capability, request.video_codec, request.chroma);
        if (!candidate.supports_request) {
            candidate.unavailable_reason = UnavailableReason(candidate, request);
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

EncoderDeviceResolution ResolveEncoderDevice(std::span<const EncoderDeviceCandidate> candidates,
                                             const exosnap::engine::EncoderDevicePreference& preference,
                                             int64_t capture_adapter_luid, bool capture_adapter_known) {
    EncoderDeviceResolution result;

    if (preference.mode == exosnap::engine::EncoderDevicePreference::Mode::Explicit) {
        std::vector<size_t> matches;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (exosnap::engine::SameHardwareIdentity(FingerprintFromAdapter(candidates[i].adapter),
                                                      preference.device)) {
                matches.push_back(i);
            }
        }
        if (matches.empty()) {
            result.reason =
                EXOSNAP_TRANSLATABLE("Capabilities", "The selected encoder device is not present on this system.");
            return result;
        }
        if (matches.size() > 1) {
            // Two identical cards share every PCI fact; nothing here can tell
            // them apart, and binding to one would be a guess.
            result.reason = EXOSNAP_TRANSLATABLE(
                "Capabilities", "Multiple adapters match the selected encoder device and cannot be told apart. "
                                "Choose Auto or select the device again.");
            return result;
        }
        const EncoderDeviceCandidate& device = candidates[matches.front()];
        result.candidate_index = static_cast<long>(matches.front());
        result.validation = PopulateAssignment(result, device, candidates, capture_adapter_luid, capture_adapter_known);
        if (!device.usable()) {
            result.reason = device.unavailable_reason;
            return result;
        }
        if (capture_adapter_known) {
            result.resolved = result.validation.ok;
            result.reason = result.validation.reason;
            return result;
        }
        // Capture not chosen yet: the device is executable-at-start and the
        // engine re-validates the assignment against the actual capture adapter.
        result.resolved = true;
        return result;
    }

    // Auto. The capture adapter is the only adapter this build can encode for,
    // so Auto selects it or fails honestly -- it never picks a different GPU.
    if (!capture_adapter_known) {
        result.deferred_to_capture = true;
        result.reason = EXOSNAP_TRANSLATABLE("Capabilities",
                                             "Auto uses the capture adapter; it is resolved when recording starts.");
        return result;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].adapter.luid != capture_adapter_luid) {
            continue;
        }
        result.candidate_index = static_cast<long>(i);
        result.validation = PopulateAssignment(result, candidates[i], candidates, capture_adapter_luid, true);
        if (!result.validation.ok) {
            result.reason = result.validation.reason;
            return result;
        }
        result.resolved = true;
        return result;
    }
    result.reason = EXOSNAP_TRANSLATABLE("Capabilities", "No encoder-capable device matches the capture source.");
    return result;
}

CapabilitySet CapabilitySetForAdapter(const CapabilitySet& base, const AdapterInfo& adapter,
                                      const AdapterEncoderCapability& capability) {
    CapabilitySet result = base;
    result.gpu_adapter_name = adapter.name;
    if (base.runtime.adapter.adapter_luid != adapter.luid) {
        result.runtime.adapter.driver_version.clear();
    }
    result.runtime.adapter.adapter_luid = adapter.luid;
    result.runtime.adapter.vendor_id = adapter.vendor_id;

    result.runtime.nvidia.nvenc_codec_probed = false;
    for (const VideoCodec codec : AllVideoCodecs()) {
        result.bframe_capability[codec] = {
            {SupportLevel::NotImplemented, "Advanced encoder support has not been confirmed for this adapter."}, 0, 0};
        result.lookahead[codec] = {SupportLevel::NotImplemented,
                                   "Lookahead support has not been confirmed for this adapter."};
        result.temporal_aq[codec] = {SupportLevel::NotImplemented,
                                     "Temporal AQ support has not been confirmed for this adapter."};
    }
    if (adapter.vendor != AdapterVendor::Nvidia) {
        const std::string reason =
            EXOSNAP_TRANSLATABLE("Capabilities", "No encoder backend is implemented for this adapter in this build.");
        for (const VideoCodec codec : AllVideoCodecs()) {
            result.video_codecs[codec] = {SupportLevel::NotImplemented, reason};
            result.chroma444[codec] = {SupportLevel::NotImplemented, reason};
        }
    } else {
        NvidiaRuntimeFacts& facts = result.runtime.nvidia;
        facts.nvenc_codec_probed = capability.probed;
        facts.nvenc_h264 = capability.h264;
        facts.nvenc_hevc = capability.hevc;
        facts.nvenc_av1 = capability.av1;
        facts.nvenc_yuv444_h264 = capability.yuv444_h264;
        facts.nvenc_yuv444_hevc = capability.yuv444_hevc;
        ApplyNvencCodecSupport(result, facts);
        ApplyNvencYuv444Support(result, facts);
        facts.nvenc_adv_h264 = {capability.max_bframes_h264, capability.bframe_ref_mode_h264, capability.lookahead_h264,
                                capability.temporal_aq_h264};
        facts.nvenc_adv_hevc = {capability.max_bframes_hevc, capability.bframe_ref_mode_hevc, capability.lookahead_hevc,
                                capability.temporal_aq_hevc};
        facts.nvenc_adv_av1 = {capability.max_bframes_av1, capability.bframe_ref_mode_av1, capability.lookahead_av1,
                               capability.temporal_aq_av1};
        ApplyNvencAdvancedEncodeSupport(result, facts);
    }

    // A combo override can pin an unavailable codec back to Available; scoped
    // to this adapter, that would resurrect exactly the path the probe denied.
    for (auto it = result.combo_overrides.begin(); it != result.combo_overrides.end();) {
        if (!IsSelectable(result.QueryVideoCodec(it->first.v))) {
            it = result.combo_overrides.erase(it);
        } else {
            ++it;
        }
    }
    return result;
}

} // namespace exosnap::capability
