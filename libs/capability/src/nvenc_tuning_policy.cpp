#include <capability/nvenc_tuning_policy.h>

#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/support_level.h>
#include <cstdint>
#include <exosnap/engine/backend_tuning.h>
#include <exosnap/engine/codec_types.h>

#include <algorithm>

namespace exosnap::capability {

NvencTuningResolution ResolveNvencTuning(const engine::NvencTuning& requested, const CapabilitySet& caps,
                                         VideoCodec codec, engine::RateControlMode rate_control,
                                         NvencLookaheadPolicy policy, const NvencLookaheadPolicyContext& context) {
    NvencTuningResolution result;
    result.tuning = requested;
    const auto& facts = caps.runtime.nvidia;
    const bool codec_probed = codec == VideoCodec::H264   ? facts.nvenc_h264
                              : codec == VideoCodec::Hevc ? facts.nvenc_hevc
                                                          : facts.nvenc_av1;
    result.available = caps.runtime.adapter.vendor_id == 0x10DE && facts.nvenc_codec_probed && codec_probed &&
                       IsSelectable(caps.QueryVideoCodec(codec));
    result.reason = result.available ? "" : "NVENC support for this adapter and codec has not been confirmed.";
    const auto bframes = caps.QueryBFrames(codec);
    if (result.available && IsSelectable(bframes.annotation)) {
        result.max_bframes = std::clamp(bframes.max_bframes, 0, 31);
    }
    result.bframes_reason = result.available ? bframes.annotation.reason : result.reason;
    if (codec == VideoCodec::Av1 && result.max_bframes > 7) {
        result.max_bframes = static_cast<int>(engine::MaxNvencBframes(engine::VideoCodec::Av1, requested.b_ref_mode,
                                                                      static_cast<uint32_t>(result.max_bframes)));
        result.bframes_reason =
            "AV1 B-frame counts above 7 require hierarchical reference mode, which this SDK does not implement.";
    }
    result.tuning.bframes = std::min(requested.bframes, static_cast<uint32_t>(result.max_bframes));
    if (result.tuning.bframes > 0) {
        if ((bframes.bframe_ref_mode & 1) != 0) {
            result.b_ref_modes.push_back(engine::NvencBRefMode::Each);
        }
        if ((bframes.bframe_ref_mode & 2) != 0 && result.tuning.bframes >= 2) {
            result.b_ref_modes.push_back(engine::NvencBRefMode::Middle);
        }
    }
    if (std::find(result.b_ref_modes.begin(), result.b_ref_modes.end(), requested.b_ref_mode) ==
        result.b_ref_modes.end()) {
        result.tuning.b_ref_mode = engine::NvencBRefMode::Off;
    }
    result.b_ref_reason = !result.available                ? result.reason
                          : result.tuning.bframes == 0     ? "B-frame references require B-frames."
                          : result.b_ref_modes.size() == 1 ? "This codec and GPU do not support B-frame references."
                                                           : "";
    const auto lookahead = caps.QueryLookahead(codec);
    const bool hardware_lookahead = result.available && IsSelectable(lookahead);
    result.lookahead_max_depth = hardware_lookahead ? engine::MaxNvencLookaheadDepth(result.tuning.bframes) : 0;
    result.lookahead_supported = hardware_lookahead && result.lookahead_max_depth > 0;
    // Capability support alone does not qualify an Auto default. Keep the
    // measured hardware/output path narrow until broader evidence is available.
    const auto& output = context.output;
    result.lookahead_auto_qualified =
        result.lookahead_supported && caps.gpu_adapter_name == "NVIDIA GeForce RTX 5070 Ti" &&
        caps.runtime.adapter.driver_version == "32.0.16.1714" && codec == VideoCodec::Av1 &&
        rate_control == engine::RateControlMode::VariableBitrate && requested.preset == engine::NvencPreset::P4 &&
        output.output_width == 1920 && output.output_height == 1080 && output.frame_rate_num == 60 &&
        output.frame_rate_den == 1 && output.bit_depth == BitDepth::Bit8 && output.chroma == ChromaSubsampling::Cs420 &&
        output.color_range == ColorRange::Limited &&
        (output.hdr_mode == engine::HdrMode::Off || output.hdr_mode == engine::HdrMode::TonemapSdr) && context.cfr &&
        context.two_second_gop && requested.bframes == 0 && requested.b_ref_mode == engine::NvencBRefMode::Off &&
        !requested.spatial_aq && !requested.temporal_aq && requested.multipass == engine::NvencMultipass::SinglePass &&
        policy == NvencLookaheadPolicy::Auto;
    result.tuning.lookahead = policy == NvencLookaheadPolicy::Auto ? result.lookahead_auto_qualified
                                                                   : requested.lookahead && result.lookahead_supported;
    const uint32_t depth = policy == NvencLookaheadPolicy::Auto ? 16u : requested.lookahead_depth;
    result.tuning.lookahead_depth =
        result.tuning.lookahead ? std::clamp(depth, result.lookahead_min_depth, result.lookahead_max_depth) : 0;
    result.lookahead_reason = !result.available ? result.reason
                              : hardware_lookahead && !result.lookahead_supported
                                  ? "Reduce the B-frame count to enable Lookahead."
                                  : lookahead.reason;
    if (policy == NvencLookaheadPolicy::Auto && result.lookahead_supported) {
        result.lookahead_reason = result.lookahead_auto_qualified
                                      ? "Qualified AV1/VBR policy enables Lookahead at depth 16."
                                      : "No qualified Auto Lookahead policy applies to this configuration.";
    }
    result.spatial_aq_supported = result.available;
    result.tuning.spatial_aq = requested.spatial_aq && result.spatial_aq_supported;
    result.spatial_aq_reason = result.available ? "" : result.reason;
    const auto temporal_aq = caps.QueryTemporalAq(codec);
    result.temporal_aq_supported = result.available && IsSelectable(temporal_aq);
    result.tuning.temporal_aq = requested.temporal_aq && result.temporal_aq_supported;
    result.temporal_aq_reason = result.available ? temporal_aq.reason : result.reason;
    result.multipass_supported = result.available && (rate_control == engine::RateControlMode::VariableBitrate ||
                                                      rate_control == engine::RateControlMode::ConstantBitrate);
    if (!result.multipass_supported || (requested.multipass != engine::NvencMultipass::SinglePass &&
                                        requested.multipass != engine::NvencMultipass::QuarterResolution &&
                                        requested.multipass != engine::NvencMultipass::FullResolution)) {
        result.tuning.multipass = engine::NvencMultipass::SinglePass;
    }
    result.multipass_reason = !result.available             ? result.reason
                              : !result.multipass_supported ? "Multipass rate control is available with VBR or CBR."
                                                            : "";
    return result;
}

} // namespace exosnap::capability
