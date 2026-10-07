#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/nvenc_tuning_policy.h>
#include <exosnap/engine/backend_tuning.h>
#include <exosnap/engine/codec_types.h>
#include <vector>

namespace {
using namespace exosnap::capability;
using namespace exosnap::engine;

CapabilitySet SupportedCaps() {
    auto caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.runtime.adapter.vendor_id = 0x10DE;
    caps.runtime.nvidia.nvenc_codec_probed = true;
    caps.runtime.nvidia.nvenc_h264 = true;
    caps.runtime.nvidia.nvenc_adv_h264 = {3, 2, true, true};
    ApplyNvencAdvancedEncodeSupport(caps, caps.runtime.nvidia);
    return caps;
}
} // namespace

TEST(NvencTuningPolicy, UnprobedAndOtherBackendsFailClosed) {
    NvencTuning requested;
    requested.bframes = 3;
    requested.lookahead = true;
    requested.spatial_aq = true;
    requested.temporal_aq = true;
    requested.multipass = NvencMultipass::FullResolution;
    auto caps = SupportedCaps();
    caps.runtime.nvidia.nvenc_codec_probed = false;
    auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::VariableBitrate);
    EXPECT_FALSE(result.available);
    EXPECT_EQ(result.tuning.bframes, 0u);
    EXPECT_FALSE(result.tuning.lookahead);
    EXPECT_FALSE(result.tuning.spatial_aq);
    EXPECT_FALSE(result.tuning.temporal_aq);
    EXPECT_EQ(result.tuning.multipass, NvencMultipass::SinglePass);
    caps = SupportedCaps();
    caps.runtime.adapter.vendor_id = 0x8086;
    EXPECT_FALSE(
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::VariableBitrate)
            .available);
}

TEST(NvencTuningPolicy, ReconcilesCountReferenceAndProductDepthTogether) {
    auto caps = SupportedCaps();
    NvencTuning requested;
    requested.bframes = 20;
    requested.b_ref_mode = NvencBRefMode::Middle;
    requested.lookahead = true;
    requested.lookahead_depth = 32;
    auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantQuality);
    EXPECT_TRUE(result.available);
    EXPECT_EQ(result.tuning.bframes, 3u);
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Middle);
    EXPECT_EQ(result.tuning.lookahead_depth, 16u);
    EXPECT_EQ(result.lookahead_min_depth, 1u);
    EXPECT_EQ(result.lookahead_max_depth, 16u);
    requested.bframes = 0;
    result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantQuality);
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Off);
    EXPECT_EQ(result.b_ref_modes, std::vector<NvencBRefMode>{NvencBRefMode::Off});
}

TEST(NvencTuningPolicy, ReferenceCapabilityBitsExposeOnlyKnownSupportedModes) {
    auto caps = SupportedCaps();
    NvencTuning requested;
    requested.bframes = 3;
    requested.b_ref_mode = NvencBRefMode::Each;
    auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantQuality);
    ASSERT_EQ(result.b_ref_modes.size(), 2u);
    EXPECT_EQ(result.b_ref_modes.back(), NvencBRefMode::Middle);
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Off);
    caps.bframe_capability[exosnap::capability::VideoCodec::H264].bframe_ref_mode = 1;
    result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantQuality);
    ASSERT_EQ(result.b_ref_modes.size(), 2u);
    EXPECT_EQ(result.b_ref_modes.back(), NvencBRefMode::Each);
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Each);
}

TEST(NvencTuningPolicy, AqDoesNotNeedLookaheadAndMultipassOnlyUsesBitrateModes) {
    auto caps = SupportedCaps();
    NvencTuning requested;
    requested.spatial_aq = true;
    requested.temporal_aq = true;
    requested.multipass = NvencMultipass::QuarterResolution;
    auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantQuality);
    EXPECT_TRUE(result.tuning.spatial_aq);
    EXPECT_TRUE(result.tuning.temporal_aq);
    EXPECT_FALSE(result.tuning.lookahead);
    EXPECT_FALSE(result.multipass_supported);
    EXPECT_EQ(result.tuning.multipass, NvencMultipass::SinglePass);
    result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantBitrate);
    EXPECT_TRUE(result.multipass_supported);
    EXPECT_EQ(result.tuning.multipass, NvencMultipass::QuarterResolution);
    EXPECT_EQ(requested.multipass, NvencMultipass::QuarterResolution);
}

TEST(NvencTuningPolicy, ConservativeDefaultsStayOff) {
    auto result = ResolveNvencTuning({}, SupportedCaps(), exosnap::capability::VideoCodec::H264,
                                     RateControlMode::VariableBitrate);
    EXPECT_EQ(result.tuning.bframes, 0u);
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Off);
    EXPECT_FALSE(result.tuning.lookahead);
    EXPECT_FALSE(result.tuning.spatial_aq);
    EXPECT_FALSE(result.tuning.temporal_aq);
    EXPECT_EQ(result.tuning.multipass, NvencMultipass::SinglePass);
}

TEST(NvencTuningPolicy, CombinedAndUnknownReferenceBitsStayHonest) {
    auto caps = SupportedCaps();
    NvencTuning requested;
    requested.bframes = 3;
    requested.b_ref_mode = NvencBRefMode::Middle;
    for (const int capability : {3, 7}) {
        caps.bframe_capability[exosnap::capability::VideoCodec::H264].bframe_ref_mode = capability;
        const auto result = ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264,
                                               RateControlMode::ConstantQuality);
        EXPECT_EQ(result.b_ref_modes,
                  (std::vector<NvencBRefMode>{NvencBRefMode::Off, NvencBRefMode::Each, NvencBRefMode::Middle}));
        EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Middle);
    }
    caps.bframe_capability[exosnap::capability::VideoCodec::H264].bframe_ref_mode = 4;
    const auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::ConstantQuality);
    EXPECT_EQ(result.b_ref_modes, std::vector<NvencBRefMode>{NvencBRefMode::Off});
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Off);
}

TEST(NvencTuningPolicy, MaximumBCountLeavesNoSafeLookaheadDepth) {
    auto caps = SupportedCaps();
    caps.bframe_capability[exosnap::capability::VideoCodec::H264].max_bframes = 31;
    NvencTuning requested;
    requested.bframes = 31;
    requested.lookahead = true;
    const auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::H264, RateControlMode::VariableBitrate);
    EXPECT_EQ(result.tuning.bframes, 31u);
    EXPECT_FALSE(result.lookahead_supported);
    EXPECT_EQ(result.lookahead_max_depth, 0u);
    EXPECT_FALSE(result.tuning.lookahead);
    EXPECT_EQ(result.tuning.lookahead_depth, 0u);
    EXPECT_EQ(result.lookahead_reason, "Reduce the B-frame count to enable Lookahead.");
    EXPECT_TRUE(requested.lookahead);
}
TEST(NvencTuningPolicy, Av1RawHierarchicalMaximumDoesNotEnableUnimplementedModes) {
    auto caps = SupportedCaps();
    caps.runtime.nvidia.nvenc_av1 = true;
    caps.runtime.nvidia.nvenc_adv_av1 = {31, 7, true, true};
    ApplyNvencAdvancedEncodeSupport(caps, caps.runtime.nvidia);
    NvencTuning requested;
    requested.bframes = 31;
    requested.lookahead = true;
    requested.b_ref_mode = NvencBRefMode::Middle;
    const auto result =
        ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::Av1, RateControlMode::ConstantQuality);
    EXPECT_EQ(caps.QueryBFrames(exosnap::capability::VideoCodec::Av1).max_bframes, 31);
    EXPECT_EQ(result.max_bframes, 7);
    EXPECT_EQ(result.tuning.bframes, 7u);
    EXPECT_EQ(result.tuning.b_ref_mode, NvencBRefMode::Middle);
    EXPECT_TRUE(result.tuning.lookahead);
    EXPECT_EQ(result.tuning.lookahead_depth, 16u);
    EXPECT_FALSE(result.bframes_reason.empty());
    EXPECT_EQ(requested.bframes, 31u);
}
namespace {
CapabilitySet QualifiedLookaheadCaps() {
    auto caps = SupportedCaps();
    caps.gpu_adapter_name = "NVIDIA GeForce RTX 5070 Ti";
    caps.runtime.adapter.driver_version = "32.0.16.1714";
    caps.runtime.nvidia.nvenc_av1 = true;
    caps.runtime.nvidia.nvenc_adv_av1 = {7, 2, true, true};
    ApplyNvencCodecSupport(caps, caps.runtime.nvidia);
    ApplyNvencAdvancedEncodeSupport(caps, caps.runtime.nvidia);
    return caps;
}
NvencLookaheadPolicyContext QualifiedLookaheadOutput() {
    NvencLookaheadPolicyContext context;
    context.output.output_width = 1920;
    context.output.output_height = 1080;
    context.cfr = true;
    context.two_second_gop = true;
    return context;
}
} // namespace

TEST(NvencTuningPolicy, AutoQualifiesOnlyTheMeasuredAv1VbrPath) {
    NvencTuning requested;
    requested.lookahead_depth = 5;
    const auto resolved =
        ResolveNvencTuning(requested, QualifiedLookaheadCaps(), exosnap::capability::VideoCodec::Av1,
                           RateControlMode::VariableBitrate, NvencLookaheadPolicy::Auto, QualifiedLookaheadOutput());
    EXPECT_TRUE(resolved.tuning.lookahead);
    EXPECT_EQ(resolved.tuning.lookahead_depth, 16u);
    EXPECT_TRUE(resolved.lookahead_auto_qualified);
    EXPECT_FALSE(requested.lookahead);
    EXPECT_EQ(requested.lookahead_depth, 5u);
}

TEST(NvencTuningPolicy, ExplicitLookaheadOverridesAutoQualification) {
    NvencTuning requested;
    const auto caps = QualifiedLookaheadCaps();
    const auto context = QualifiedLookaheadOutput();
    EXPECT_FALSE(ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::Av1,
                                    RateControlMode::VariableBitrate, NvencLookaheadPolicy::Explicit, context)
                     .tuning.lookahead);
    requested.lookahead = true;
    requested.lookahead_depth = 5;
    const auto on = ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::Av1,
                                       RateControlMode::ConstantQuality, NvencLookaheadPolicy::Explicit, context);
    EXPECT_TRUE(on.tuning.lookahead);
    EXPECT_EQ(on.tuning.lookahead_depth, 5u);
    EXPECT_FALSE(on.lookahead_auto_qualified);
}

TEST(NvencTuningPolicy, AutoDoesNotGeneralizeAcrossCodecsOrRateControl) {
    const auto caps = QualifiedLookaheadCaps();
    const auto context = QualifiedLookaheadOutput();
    NvencTuning requested;
    requested.lookahead = true;
    for (const auto codec : AllVideoCodecs()) {
        for (const auto rc : {RateControlMode::ConstantQuality, RateControlMode::ConstantBitrate}) {
            EXPECT_FALSE(
                ResolveNvencTuning(requested, caps, codec, rc, NvencLookaheadPolicy::Auto, context).tuning.lookahead);
        }
        if (codec != exosnap::capability::VideoCodec::Av1) {
            EXPECT_FALSE(ResolveNvencTuning(requested, caps, codec, RateControlMode::VariableBitrate,
                                            NvencLookaheadPolicy::Auto, context)
                             .tuning.lookahead);
        }
    }
}

TEST(NvencTuningPolicy, AutoRejectsUnknownOrUnqualifiedOutputAndHardware) {
    auto caps = QualifiedLookaheadCaps();
    auto context = QualifiedLookaheadOutput();
    NvencTuning requested;
    const auto resolve = [&] {
        return ResolveNvencTuning(requested, caps, exosnap::capability::VideoCodec::Av1,
                                  RateControlMode::VariableBitrate, NvencLookaheadPolicy::Auto, context);
    };
    context = {};
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.cfr = false;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.two_second_gop = false;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.output.output_width = 3840;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.output.frame_rate_num = 120;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.output.bit_depth = exosnap::capability::BitDepth::Bit10;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.output.chroma = exosnap::capability::ChromaSubsampling::Cs444;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.output.hdr_mode = HdrMode::Hdr10;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context.output.hdr_mode = static_cast<HdrMode>(999);
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    context.output.color_range = ColorRange::Full;
    EXPECT_FALSE(resolve().tuning.lookahead);
    context = QualifiedLookaheadOutput();
    caps.gpu_adapter_name = "NVIDIA GeForce RTX 4090";
    EXPECT_FALSE(resolve().tuning.lookahead);
    caps = QualifiedLookaheadCaps();
    caps.runtime.adapter.driver_version.clear();
    EXPECT_FALSE(resolve().tuning.lookahead);
    caps = QualifiedLookaheadCaps();
    caps.lookahead[exosnap::capability::VideoCodec::Av1] = {SupportLevel::NotImplemented, "unsupported"};
    EXPECT_FALSE(resolve().tuning.lookahead);
    caps = QualifiedLookaheadCaps();
    requested.preset = NvencPreset::P7;
    EXPECT_FALSE(resolve().tuning.lookahead);
    requested = {};
    requested.bframes = 2;
    EXPECT_FALSE(resolve().tuning.lookahead);
    requested = {};
    requested.spatial_aq = true;
    EXPECT_FALSE(resolve().tuning.lookahead);
    requested = {};
    requested.temporal_aq = true;
    EXPECT_FALSE(resolve().tuning.lookahead);
    requested = {};
    requested.multipass = NvencMultipass::FullResolution;
    EXPECT_FALSE(resolve().tuning.lookahead);
}
