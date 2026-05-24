#include <capability/resolver.h>

#include <capability/config_types.h>

#include <utility>

namespace exosnap::capability {
namespace {

std::string Stringify(Container value) {
    return std::string(ToString(value));
}

std::string Stringify(VideoCodec value) {
    return std::string(ToString(value));
}

std::string Stringify(AudioCodec value) {
    return std::string(ToString(value));
}

std::string Stringify(ChromaSubsampling value) {
    return std::string(ToString(value));
}

std::string Stringify(BitDepth value) {
    return std::string(ToString(value));
}

AudioCodec PreferredAudioCodecForContainer(Container container) {
    switch (container) {
    case Container::Matroska:
        return AudioCodec::Opus;
    case Container::WebM:
        return AudioCodec::Opus;
    case Container::Mp4:
        return AudioCodec::AacMf;
    }
    return AudioCodec::AacMf;
}

VideoCodec PreferredVideoCodecForContainer(Container container) {
    switch (container) {
    case Container::WebM:
        return VideoCodec::Av1Nvenc;
    case Container::Matroska:
        return VideoCodec::Av1Nvenc;
    case Container::Mp4:
        return VideoCodec::H264Nvenc;
    }
    return VideoCodec::H264Nvenc;
}

SupportAnnotation QueryResolvedCombo(const CapabilitySet& caps, const UserRecorderConfig& config) {
    return caps.QueryCombo(config.container, config.video_codec, config.audio_codec, config.chroma, config.bit_depth);
}

void AddWarningForUnvalidated(ResolveResult& result, const SupportAnnotation& annotation) {
    if (annotation.level == SupportLevel::ValidUnvalidated) {
        result.warnings.push_back(
            Warning{"combo.valid_unvalidated", annotation.reason.empty()
                                                   ? "Selected combination is valid but not validated on this system."
                                                   : annotation.reason});
    }
}

void AddInvalid(ResolveResult& result, std::string field, std::string message) {
    result.succeeded = false;
    result.invalidity.push_back(InvalidReason{
        std::move(field),
        std::move(message),
    });
}

bool ValidateResolution(const CapabilitySet& caps, const UserRecorderConfig& config, ResolveResult& result) {
    if (config.frame_rate_num == 0 || config.frame_rate_den == 0) {
        AddInvalid(result, "frame_rate", "frame_rate_num and frame_rate_den must both be non-zero.");
        return false;
    }

    const ResolutionConstraint& constraint = caps.resolution_constraint;
    const uint32_t width = config.output_width;
    const uint32_t height = config.output_height;

    if (constraint.max_width != 0 && width > constraint.max_width) {
        AddInvalid(result, "output_width", "Requested width exceeds maximum supported width.");
        return false;
    }
    if (constraint.max_height != 0 && height > constraint.max_height) {
        AddInvalid(result, "output_height", "Requested height exceeds maximum supported height.");
        return false;
    }

    if (constraint.must_be_even) {
        if (width != 0 && (width % 2u) != 0u) {
            AddInvalid(result, "output_width", "Width must be even when non-zero.");
            return false;
        }
        if (height != 0 && (height % 2u) != 0u) {
            AddInvalid(result, "output_height", "Height must be even when non-zero.");
            return false;
        }
    }

    return true;
}

bool FinalizeSelectableResult(const CapabilitySet& caps, ResolveResult& result, const std::string& field) {
    if (!ValidateResolution(caps, result.resolved_config, result)) {
        return false;
    }

    const SupportAnnotation combo = QueryResolvedCombo(caps, result.resolved_config);
    if (!IsSelectable(combo.level)) {
        const std::string message =
            combo.reason.empty() ? "The requested configuration is not selectable." : combo.reason;
        AddInvalid(result, field, message);
        return false;
    }

    result.succeeded = true;
    AddWarningForUnvalidated(result, combo);
    return true;
}

} // namespace

RequestedChange::RequestedChange(Value value) : value_(std::move(value)) {
}

RequestedChange RequestedChange::ForContainer(Container value) {
    return RequestedChange(value);
}

RequestedChange RequestedChange::ForVideoCodec(VideoCodec value) {
    return RequestedChange(value);
}

RequestedChange RequestedChange::ForAudioCodec(AudioCodec value) {
    return RequestedChange(value);
}

RequestedChange RequestedChange::ForChroma(ChromaSubsampling value) {
    return RequestedChange(value);
}

RequestedChange RequestedChange::ForBitDepth(BitDepth value) {
    return RequestedChange(value);
}

RequestedChange RequestedChange::ForResolution(uint32_t width, uint32_t height) {
    return RequestedChange(ResolutionValue{width, height});
}

const RequestedChange::Value& RequestedChange::value() const noexcept {
    return value_;
}

SettingsResolver::SettingsResolver(const CapabilitySet& caps) : caps_(caps) {
}

ResolveResult SettingsResolver::ResolveChange(const UserRecorderConfig& current, const RequestedChange& change) const {
    ResolveResult result;
    result.resolved_config = current;

    if (const auto* container = std::get_if<Container>(&change.value())) {
        result.resolved_config.container = *container;
        SupportAnnotation combo = QueryResolvedCombo(caps_, result.resolved_config);

        if (!IsSelectable(combo.level)) {
            // Try preferred audio for the new container first.
            const AudioCodec preferred_audio = PreferredAudioCodecForContainer(*container);
            if (result.resolved_config.audio_codec != preferred_audio) {
                UserRecorderConfig candidate = result.resolved_config;
                candidate.audio_codec = preferred_audio;
                const SupportAnnotation audio_fallback = QueryResolvedCombo(caps_, candidate);
                if (IsSelectable(audio_fallback.level)) {
                    result.adjustments.push_back(
                        Adjustment{"audio_codec", Stringify(result.resolved_config.audio_codec),
                                   Stringify(preferred_audio), "Adjusted to preferred codec for selected container."});
                    result.resolved_config = candidate;
                    FinalizeSelectableResult(caps_, result, "container");
                    return result;
                }
            }

            // Audio-only fallback did not work: also try preferred video + preferred audio.
            const VideoCodec preferred_video = PreferredVideoCodecForContainer(*container);
            if (result.resolved_config.video_codec != preferred_video) {
                UserRecorderConfig candidate = result.resolved_config;
                candidate.video_codec = preferred_video;
                candidate.audio_codec = preferred_audio;
                const SupportAnnotation av_fallback = QueryResolvedCombo(caps_, candidate);
                if (IsSelectable(av_fallback.level)) {
                    if (result.resolved_config.video_codec != preferred_video) {
                        result.adjustments.push_back(Adjustment{
                            "video_codec", Stringify(result.resolved_config.video_codec), Stringify(preferred_video),
                            "Adjusted video codec to preferred for selected container."});
                    }
                    if (result.resolved_config.audio_codec != preferred_audio) {
                        result.adjustments.push_back(Adjustment{
                            "audio_codec", Stringify(result.resolved_config.audio_codec), Stringify(preferred_audio),
                            "Adjusted audio codec to preferred for selected container."});
                    }
                    result.resolved_config = candidate;
                    FinalizeSelectableResult(caps_, result, "container");
                    return result;
                }
            }

            AddInvalid(result, "container",
                       "Container requires codec fallback, but no selectable combination was found.");
            return result;
        }

        if (!FinalizeSelectableResult(caps_, result, "container")) {
            return result;
        }
        return result;
    }

    if (const auto* video = std::get_if<VideoCodec>(&change.value())) {
        result.resolved_config.video_codec = *video;
        SupportAnnotation combo = QueryResolvedCombo(caps_, result.resolved_config);

        if (!IsSelectable(combo.level) && result.resolved_config.chroma != ChromaSubsampling::Cs420) {
            const SupportAnnotation fallback_chroma = caps_.QueryChroma(ChromaSubsampling::Cs420);
            if (!IsSelectable(fallback_chroma.level)) {
                AddInvalid(result, "chroma", "Fallback chroma 4:2:0 is not selectable.");
                return result;
            }

            result.adjustments.push_back(Adjustment{"chroma", Stringify(result.resolved_config.chroma),
                                                    Stringify(ChromaSubsampling::Cs420),
                                                    "Adjusted to supported chroma for selected video codec."});
            result.resolved_config.chroma = ChromaSubsampling::Cs420;
            combo = QueryResolvedCombo(caps_, result.resolved_config);
        }

        if (!IsSelectable(combo.level) && result.resolved_config.bit_depth != BitDepth::Bit8) {
            const SupportAnnotation fallback_bit_depth = caps_.QueryBitDepth(BitDepth::Bit8);
            if (!IsSelectable(fallback_bit_depth.level)) {
                AddInvalid(result, "bit_depth", "Fallback bit depth 8-bit is not selectable.");
                return result;
            }

            result.adjustments.push_back(Adjustment{"bit_depth", Stringify(result.resolved_config.bit_depth),
                                                    Stringify(BitDepth::Bit8),
                                                    "Adjusted to supported bit depth for selected video codec."});
            result.resolved_config.bit_depth = BitDepth::Bit8;
            combo = QueryResolvedCombo(caps_, result.resolved_config);
        }

        if (!IsSelectable(combo.level)) {
            AddInvalid(result, "video_codec",
                       combo.reason.empty() ? "Requested video codec combination is not selectable." : combo.reason);
            return result;
        }

        if (!ValidateResolution(caps_, result.resolved_config, result)) {
            return result;
        }
        result.succeeded = true;
        AddWarningForUnvalidated(result, combo);
        return result;
    }

    if (const auto* audio = std::get_if<AudioCodec>(&change.value())) {
        result.resolved_config.audio_codec = *audio;
        if (!FinalizeSelectableResult(caps_, result, "audio_codec")) {
            return result;
        }
        return result;
    }

    if (const auto* chroma = std::get_if<ChromaSubsampling>(&change.value())) {
        result.resolved_config.chroma = *chroma;
        if (!FinalizeSelectableResult(caps_, result, "chroma")) {
            return result;
        }
        return result;
    }

    if (const auto* bit_depth = std::get_if<BitDepth>(&change.value())) {
        result.resolved_config.bit_depth = *bit_depth;
        if (!FinalizeSelectableResult(caps_, result, "bit_depth")) {
            return result;
        }
        return result;
    }

    if (const auto* resolution = std::get_if<RequestedChange::ResolutionValue>(&change.value())) {
        result.resolved_config.output_width = resolution->width;
        result.resolved_config.output_height = resolution->height;
        if (!FinalizeSelectableResult(caps_, result, "resolution")) {
            return result;
        }
        return result;
    }

    AddInvalid(result, "change", "Unsupported change type.");
    return result;
}

ResolveResult SettingsResolver::ValidateConfig(const UserRecorderConfig& config) const {
    ResolveResult result;
    result.resolved_config = config;

    if (!ValidateResolution(caps_, result.resolved_config, result)) {
        return result;
    }

    SupportAnnotation combo = QueryResolvedCombo(caps_, result.resolved_config);

    if (!IsSelectable(combo.level)) {
        const AudioCodec preferred = PreferredAudioCodecForContainer(result.resolved_config.container);
        if (result.resolved_config.audio_codec != preferred) {
            UserRecorderConfig candidate = result.resolved_config;
            const ChromaSubsampling original_chroma = candidate.chroma;
            const BitDepth original_bit_depth = candidate.bit_depth;
            candidate.audio_codec = preferred;
            SupportAnnotation fallback_combo = QueryResolvedCombo(caps_, candidate);

            bool chroma_fallback_applied = false;
            bool bit_depth_fallback_applied = false;

            if (!IsSelectable(fallback_combo.level) && candidate.chroma != ChromaSubsampling::Cs420) {
                const SupportAnnotation fallback_chroma = caps_.QueryChroma(ChromaSubsampling::Cs420);
                if (!IsSelectable(fallback_chroma.level)) {
                    AddInvalid(result, "chroma", "Fallback chroma 4:2:0 is not selectable.");
                    return result;
                }
                candidate.chroma = ChromaSubsampling::Cs420;
                chroma_fallback_applied = true;
                fallback_combo = QueryResolvedCombo(caps_, candidate);
            }

            if (!IsSelectable(fallback_combo.level) && candidate.bit_depth != BitDepth::Bit8) {
                const SupportAnnotation fallback_bit_depth = caps_.QueryBitDepth(BitDepth::Bit8);
                if (!IsSelectable(fallback_bit_depth.level)) {
                    AddInvalid(result, "bit_depth", "Fallback bit depth 8-bit is not selectable.");
                    return result;
                }
                candidate.bit_depth = BitDepth::Bit8;
                bit_depth_fallback_applied = true;
                fallback_combo = QueryResolvedCombo(caps_, candidate);
            }

            if (IsSelectable(fallback_combo.level)) {
                result.adjustments.push_back(Adjustment{"audio_codec", Stringify(result.resolved_config.audio_codec),
                                                        Stringify(preferred),
                                                        "Adjusted to preferred codec for selected container."});

                if (chroma_fallback_applied) {
                    result.adjustments.push_back(Adjustment{"chroma", Stringify(original_chroma),
                                                            Stringify(ChromaSubsampling::Cs420),
                                                            "Adjusted to supported chroma while validating profile."});
                }

                if (bit_depth_fallback_applied) {
                    result.adjustments.push_back(
                        Adjustment{"bit_depth", Stringify(original_bit_depth), Stringify(BitDepth::Bit8),
                                   "Adjusted to supported bit depth while validating profile."});
                }

                result.resolved_config = candidate;
                combo = fallback_combo;
            } else {
                AddInvalid(result, "audio_codec",
                           "Container requires audio fallback, but preferred codec is not selectable: " +
                               fallback_combo.reason);
                return result;
            }
        }
    }

    if (!IsSelectable(combo.level) && result.resolved_config.chroma != ChromaSubsampling::Cs420) {
        const SupportAnnotation fallback_chroma = caps_.QueryChroma(ChromaSubsampling::Cs420);
        if (!IsSelectable(fallback_chroma.level)) {
            AddInvalid(result, "chroma", "Fallback chroma 4:2:0 is not selectable.");
            return result;
        }
        result.adjustments.push_back(Adjustment{"chroma", Stringify(result.resolved_config.chroma),
                                                Stringify(ChromaSubsampling::Cs420),
                                                "Adjusted to supported chroma for selected video codec."});
        result.resolved_config.chroma = ChromaSubsampling::Cs420;
        combo = QueryResolvedCombo(caps_, result.resolved_config);
    }

    if (!IsSelectable(combo.level) && result.resolved_config.bit_depth != BitDepth::Bit8) {
        const SupportAnnotation fallback_bit_depth = caps_.QueryBitDepth(BitDepth::Bit8);
        if (!IsSelectable(fallback_bit_depth.level)) {
            AddInvalid(result, "bit_depth", "Fallback bit depth 8-bit is not selectable.");
            return result;
        }
        result.adjustments.push_back(Adjustment{"bit_depth", Stringify(result.resolved_config.bit_depth),
                                                Stringify(BitDepth::Bit8),
                                                "Adjusted to supported bit depth for selected video codec."});
        result.resolved_config.bit_depth = BitDepth::Bit8;
        combo = QueryResolvedCombo(caps_, result.resolved_config);
    }

    if (!IsSelectable(combo.level)) {
        AddInvalid(result, "config", combo.reason.empty() ? "Configuration is not selectable." : combo.reason);
        return result;
    }

    result.succeeded = true;
    AddWarningForUnvalidated(result, combo);
    return result;
}

} // namespace exosnap::capability
