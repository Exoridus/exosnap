#include <gtest/gtest.h>

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>
#include <capability/config_types.h>
#include <capability/support_level.h>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/encoder_device.h>
#include <vector>

#include <capability/capability_builder.h>
#include <capability/encoder_device_resolver.h>

#include <string>

namespace {

using exosnap::capability::AdapterEncoderCapability;
using exosnap::capability::AdapterInfo;
using exosnap::capability::AdapterKind;
using exosnap::capability::AdapterVendor;
using exosnap::capability::BuildEncoderDeviceCandidates;
using exosnap::capability::CapabilitySetForAdapter;
using exosnap::capability::EncoderDeviceRequest;
using exosnap::capability::ResolveEncoderDevice;
using exosnap::capability::VideoCodec;
using exosnap::engine::EncoderBackend;
using exosnap::engine::EncoderDevicePreference;

AdapterInfo MakeAdapter(const char* name, AdapterVendor vendor, int64_t luid, uint32_t device_id = 0) {
    AdapterInfo info;
    info.name = name;
    info.vendor = vendor;
    info.vendor_id = vendor == AdapterVendor::Nvidia  ? 0x10DEu
                     : vendor == AdapterVendor::Intel ? 0x8086u
                     : vendor == AdapterVendor::Amd   ? 0x1002u
                                                      : 0x1414u;
    info.device_id = device_id != 0 ? device_id : static_cast<uint32_t>(0x1000 + luid);
    info.subsystem_id = static_cast<uint32_t>(0x2000 + luid);
    info.luid = luid;
    info.kind = AdapterKind::Discrete;
    return info;
}

AdapterEncoderCapability NvencCapability(bool av1 = true, bool hevc = true, bool h264 = true) {
    AdapterEncoderCapability capability;
    capability.probed = true;
    capability.backend_label = "NVENC";
    capability.provenance = "probed via NVENC encode GUIDs";
    capability.h264 = h264;
    capability.hevc = hevc;
    capability.av1 = av1;
    capability.yuv444_h264 = true;
    capability.yuv444_hevc = true;
    return capability;
}

std::vector<exosnap::capability::EncoderDeviceCandidate>
Candidates(const std::vector<AdapterInfo>& adapters, const std::vector<AdapterEncoderCapability>& capabilities,
           EncoderDeviceRequest request = {}) {
    return BuildEncoderDeviceCandidates(adapters, capabilities, request);
}

} // namespace

TEST(EncoderDeviceResolver, AutoChoosesTheCaptureAdapterNotTheFirstNvidia) {
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1),
                                            MakeAdapter("RTX 4090", AdapterVendor::Nvidia, 2)};
    const auto candidates = Candidates(adapters, {NvencCapability(), NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, EncoderDevicePreference{}, /*capture=*/2, true);
    ASSERT_TRUE(resolution.resolved);
    ASSERT_GE(resolution.candidate_index, 0);
    EXPECT_EQ(candidates[static_cast<size_t>(resolution.candidate_index)].adapter.luid, 2);
}

TEST(EncoderDeviceResolver, AutoFailsRatherThanChoosingAnotherGpuWhenCaptureHasNoBackend) {
    const std::vector<AdapterInfo> adapters{MakeAdapter("UHD 770", AdapterVendor::Intel, 1),
                                            MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 2)};
    const auto candidates = Candidates(adapters, {AdapterEncoderCapability{}, NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, EncoderDevicePreference{}, /*capture=*/1, true);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_FALSE(resolution.reason.empty());
}

TEST(EncoderDeviceResolver, AutoDefersWhenTheCaptureAdapterIsNotKnownYet) {
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1)};
    const auto candidates = Candidates(adapters, {NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, EncoderDevicePreference{}, 0, false);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_TRUE(resolution.deferred_to_capture);
}

TEST(EncoderDeviceResolver, AutoRejectsTheCaptureAdapterWhenTheCodecIsUnsupported) {
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 4090", AdapterVendor::Nvidia, 1)};
    const auto candidates = Candidates(adapters, {NvencCapability(/*av1=*/false)});
    const auto resolution = ResolveEncoderDevice(candidates, EncoderDevicePreference{}, 1, true);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_NE(resolution.reason.find("AV1"), std::string::npos);
}

TEST(EncoderDeviceResolver, ExplicitSupportedDeviceResolves) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    preference.device = {0x10DEu, 0x2001u, 0x2002u, "RTX 5070 Ti"};
    const std::vector<AdapterInfo> adapters{MakeAdapter("UHD 770", AdapterVendor::Intel, 1),
                                            MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 2, 0x2001u)};
    const auto candidates = Candidates(adapters, {AdapterEncoderCapability{}, NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/2, true);
    ASSERT_TRUE(resolution.resolved);
    EXPECT_EQ(candidates[static_cast<size_t>(resolution.candidate_index)].adapter.luid, 2);
}

TEST(EncoderDeviceResolver, ExplicitCrossAdapterSelectionFailsHonestly) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    preference.device = {0x10DEu, 0x1002u, 0x2002u, "RTX 4090"};
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1),
                                            MakeAdapter("RTX 4090", AdapterVendor::Nvidia, 2)};
    const auto candidates = Candidates(adapters, {NvencCapability(), NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/1, true);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_NE(resolution.reason.find("cross-adapter"), std::string::npos);
}

TEST(EncoderDeviceResolver, ExplicitAmbiguousIdenticalAdaptersNeverGuesses) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    preference.device = {0x10DEu, 0x3000u, 0x4000u, "RTX 4090"};
    AdapterInfo first = MakeAdapter("RTX 4090", AdapterVendor::Nvidia, 1, 0x3000u);
    AdapterInfo second = MakeAdapter("RTX 4090", AdapterVendor::Nvidia, 2, 0x3000u);
    first.subsystem_id = 0x4000u;
    second.subsystem_id = 0x4000u;
    const std::vector<AdapterInfo> adapters{first, second};
    const auto candidates = Candidates(adapters, {NvencCapability(), NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/1, true);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_NE(resolution.reason.find("cannot be told apart"), std::string::npos);
}

TEST(EncoderDeviceResolver, ExplicitMissingDeviceFailsWithoutRedirecting) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    preference.device = {0x10DEu, 0x9999u, 0x9999u, "RTX 5090"};
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1)};
    const auto candidates = Candidates(adapters, {NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/1, true);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_NE(resolution.reason.find("not present"), std::string::npos);
}

TEST(EncoderDeviceResolver, ExplicitNonNvidiaDeviceIsAvailableButHasNoBackend) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    const std::vector<AdapterInfo> adapters{MakeAdapter("UHD 770", AdapterVendor::Intel, 1)};
    preference.device = {0x8086u, 0x1001u, 0x2001u, "UHD 770"};
    const auto candidates = Candidates(adapters, {AdapterEncoderCapability{}});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/1, true);
    EXPECT_FALSE(resolution.resolved);
    EXPECT_NE(resolution.reason.find("No encoder backend"), std::string::npos);
}

TEST(EncoderDeviceResolver, FourFourFourUnsupportedRemovesTheCandidate) {
    EncoderDeviceRequest request;
    request.chroma = exosnap::capability::ChromaSubsampling::Cs444;
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1)};
    auto capability = NvencCapability();
    capability.yuv444_hevc = false;
    const auto candidates = Candidates(adapters, {capability}, request);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_FALSE(candidates[0].usable());
    EXPECT_FALSE(candidates[0].unavailable_reason.empty());
}

// ===========================================================================
// Pipeline adapter assignment: capture, processing and encoder as three
// separately representable roles. The 0.10 build executes only the
// all-on-one-adapter topology, so these tests pin that the assignment is never
// collapsed back to the capture adapter when transport or backend is absent.
// ===========================================================================

namespace {

using exosnap::engine::CaptureSurfaceTransport;
using exosnap::engine::CaptureSurfaceTransportFor;
using exosnap::engine::PipelineAdapterIdentity;
using exosnap::engine::PipelineAssignmentFacts;
using exosnap::engine::ValidatePipelineAssignment;

PipelineAdapterIdentity Identity(int64_t luid, uint32_t vendor_id) {
    PipelineAdapterIdentity identity;
    identity.known = true;
    identity.luid = luid;
    identity.vendor_id = vendor_id;
    return identity;
}

} // namespace

TEST(PipelineAdapterAssignment, AutoSingleNvidiaAssignsAllThreeRolesToTheCaptureAdapter) {
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 7)};
    const auto candidates = Candidates(adapters, {NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, EncoderDevicePreference{}, /*capture=*/7, true);

    ASSERT_TRUE(resolution.resolved);
    ASSERT_TRUE(resolution.validation.ok);
    EXPECT_TRUE(resolution.assignment.capture.known);
    EXPECT_EQ(resolution.assignment.capture.luid, 7);
    EXPECT_EQ(resolution.assignment.capture.vendor_id, 0x10DEu);
    EXPECT_EQ(resolution.assignment.processing.luid, 7);
    EXPECT_EQ(resolution.assignment.encoder.luid, 7);
    EXPECT_EQ(resolution.assignment.encoder.backend, EncoderBackend::Nvenc);
}

TEST(PipelineAdapterAssignment, ExplicitSameNvidiaAdapterKeepsAllThreeOnOneGpu) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    preference.device =
        exosnap::capability::FingerprintFromAdapter(MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 2, 0x2001u));
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 2, 0x2001u)};
    const auto candidates = Candidates(adapters, {NvencCapability()});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/2, true);

    ASSERT_TRUE(resolution.resolved);
    ASSERT_TRUE(resolution.validation.ok);
    EXPECT_EQ(resolution.assignment.capture.luid, 2);
    EXPECT_EQ(resolution.assignment.processing.luid, 2);
    EXPECT_EQ(resolution.assignment.encoder.luid, 2);
}

TEST(PipelineAdapterAssignment, ExplicitCrossVendorDeviceIsRepresentedButNotExecutable) {
    EncoderDevicePreference preference;
    preference.mode = EncoderDevicePreference::Mode::Explicit;
    preference.device = {0x8086u, 0x1002u, 0x2002u, "UHD 770"};
    const std::vector<AdapterInfo> adapters{MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1),
                                            MakeAdapter("UHD 770", AdapterVendor::Intel, 2)};
    const auto candidates = Candidates(adapters, {NvencCapability(), AdapterEncoderCapability{}});
    const auto resolution = ResolveEncoderDevice(candidates, preference, /*capture=*/1, true);

    EXPECT_FALSE(resolution.resolved);
    EXPECT_FALSE(resolution.reason.empty());
    // The desired topology survives in the assignment instead of collapsing
    // back onto the capture adapter.
    ASSERT_TRUE(resolution.assignment.processing.known);
    EXPECT_EQ(resolution.assignment.capture.luid, 1);
    EXPECT_EQ(resolution.assignment.capture.vendor_id, 0x10DEu);
    EXPECT_EQ(resolution.assignment.processing.luid, 2);
    EXPECT_EQ(resolution.assignment.encoder.luid, 2);
    EXPECT_EQ(resolution.assignment.encoder.vendor_id, 0x8086u);
    EXPECT_EQ(resolution.assignment.encoder.backend, EncoderBackend::None);
}

TEST(PipelineAdapterAssignment, SyntheticCrossAdapterFactsCanValidate) {
    PipelineAssignmentFacts facts;
    facts.assignment.capture = Identity(1, 0x10DEu);
    facts.assignment.processing = Identity(2, 0x8086u);
    facts.assignment.encoder = Identity(2, 0x8086u);
    facts.transport = CaptureSurfaceTransport::CrossAdapterShared;
    facts.processing_executable = true;
    facts.encoder_executable = true;

    const auto validation = ValidatePipelineAssignment(facts);
    EXPECT_TRUE(validation.ok) << validation.reason;
}

TEST(PipelineAdapterAssignment, ProductionPolicyRejectsSplitTransport) {
    PipelineAssignmentFacts facts;
    facts.assignment.capture = Identity(1, 0x10DEu);
    facts.assignment.processing = Identity(2, 0x8086u);
    facts.assignment.encoder = Identity(2, 0x8086u);
    facts.transport = CaptureSurfaceTransportFor(facts.assignment.capture, facts.assignment.processing);
    facts.processing_executable = true;
    facts.encoder_executable = true;

    const auto validation = ValidatePipelineAssignment(facts);
    EXPECT_FALSE(validation.ok);
    EXPECT_NE(validation.reason.find("cross-adapter"), std::string::npos);
}

TEST(PipelineAdapterAssignment, ProcessingAndEncoderStaySeparatelyRepresentable) {
    PipelineAssignmentFacts facts;
    facts.assignment.capture = Identity(1, 0x10DEu);
    facts.assignment.processing = Identity(1, 0x10DEu);
    facts.assignment.encoder = Identity(2, 0x8086u);
    facts.transport = CaptureSurfaceTransportFor(facts.assignment.capture, facts.assignment.processing);
    facts.processing_executable = true;
    facts.encoder_executable = false;
    facts.encoder_reason = "No encoder backend is implemented for this adapter in this build.";

    EXPECT_EQ(facts.assignment.processing.luid, 1);
    EXPECT_EQ(facts.assignment.encoder.luid, 2);
    const auto validation = ValidatePipelineAssignment(facts);
    EXPECT_FALSE(validation.ok);
    EXPECT_NE(validation.reason.find("No encoder backend"), std::string::npos);
}

TEST(CaptureSurfaceTransportPolicy, SameLuidIsSameAdapterAndDifferentIsUnsupported) {
    const PipelineAdapterIdentity nvidia = Identity(5, 0x10DEu);
    const PipelineAdapterIdentity intel = Identity(6, 0x8086u);
    EXPECT_EQ(CaptureSurfaceTransportFor(nvidia, nvidia), CaptureSurfaceTransport::SameAdapter);
    EXPECT_EQ(CaptureSurfaceTransportFor(nvidia, intel), CaptureSurfaceTransport::CrossAdapterUnsupported);
    EXPECT_EQ(CaptureSurfaceTransportFor(PipelineAdapterIdentity{}, nvidia), CaptureSurfaceTransport::Unknown);
}

TEST(EncoderDeviceFingerprint, RuntimeLuidIsNotPartOfPersistentIdentity) {
    const AdapterInfo first = MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1, 0x2001u);
    AdapterInfo second = MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 2, 0x2001u);
    second.subsystem_id = first.subsystem_id;
    EXPECT_NE(first.luid, second.luid);
    EXPECT_TRUE(exosnap::engine::SameHardwareIdentity(exosnap::capability::FingerprintFromAdapter(first),
                                                      exosnap::capability::FingerprintFromAdapter(second)));
}

TEST(EncoderDeviceCapabilityView, NonNvidiaBackendMarksEveryVideoPathUnavailable) {
    const auto base = exosnap::capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const auto view =
        CapabilitySetForAdapter(base, MakeAdapter("UHD 770", AdapterVendor::Intel, 1), AdapterEncoderCapability{});
    for (const VideoCodec codec : exosnap::capability::AllVideoCodecs()) {
        EXPECT_FALSE(exosnap::capability::IsSelectable(view.QueryVideoCodec(codec)));
    }
}

TEST(EncoderDeviceCapabilityView, ProbedNvidiaDeviceDropsExactlyTheUnsupportedCodec) {
    const auto base = exosnap::capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const auto view = CapabilitySetForAdapter(base, MakeAdapter("RTX 4090", AdapterVendor::Nvidia, 1),
                                              NvencCapability(/*av1=*/false));
    EXPECT_FALSE(exosnap::capability::IsSelectable(view.QueryVideoCodec(VideoCodec::Av1)));
    EXPECT_TRUE(exosnap::capability::IsSelectable(view.QueryVideoCodec(VideoCodec::H264)));
    EXPECT_TRUE(exosnap::capability::IsSelectable(view.QueryVideoCodec(VideoCodec::Hevc)));
}

TEST(EncoderDeviceResolver, AdvancedCapabilitiesNeverLeakFromAnotherAdapter) {
    auto base = exosnap::capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    base.runtime.nvidia.nvenc_codec_probed = true;
    base.runtime.nvidia.nvenc_h264 = true;
    base.runtime.nvidia.nvenc_adv_h264 = {3, 2, true, true};
    exosnap::capability::ApplyNvencAdvancedEncodeSupport(base, base.runtime.nvidia);
    auto scoped = CapabilitySetForAdapter(base, MakeAdapter("NVIDIA", AdapterVendor::Nvidia, 2), NvencCapability());
    EXPECT_EQ(scoped.QueryBFrames(VideoCodec::H264).max_bframes, 0);
    EXPECT_FALSE(exosnap::capability::IsSelectable(scoped.QueryLookahead(VideoCodec::H264)));
    EXPECT_FALSE(exosnap::capability::IsSelectable(scoped.QueryTemporalAq(VideoCodec::H264)));
    auto intel = CapabilitySetForAdapter(base, MakeAdapter("Intel", AdapterVendor::Intel, 3), {});
    EXPECT_EQ(intel.QueryBFrames(VideoCodec::H264).max_bframes, 0);
    EXPECT_FALSE(exosnap::capability::IsSelectable(intel.QueryLookahead(VideoCodec::H264)));
}
TEST(EncoderDeviceCapabilityView, DriverQualificationDoesNotLeakAcrossAdapters) {
    auto base = exosnap::capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    base.runtime.adapter.adapter_luid = 1;
    base.runtime.adapter.driver_version = "32.0.16.1714";
    const auto same =
        CapabilitySetForAdapter(base, MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 1), NvencCapability());
    EXPECT_EQ(same.runtime.adapter.driver_version, "32.0.16.1714");
    const auto other =
        CapabilitySetForAdapter(base, MakeAdapter("RTX 5070 Ti", AdapterVendor::Nvidia, 2), NvencCapability());
    EXPECT_TRUE(other.runtime.adapter.driver_version.empty());
}
