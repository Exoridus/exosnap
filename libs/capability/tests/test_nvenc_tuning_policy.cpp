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