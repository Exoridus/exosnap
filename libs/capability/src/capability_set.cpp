#include <capability/capability_set.h>

#include <string_view>

namespace exosnap::capability {
namespace {

int SeverityRank(SupportLevel level) noexcept {
    switch (level) {
    case SupportLevel::Available:
        return 0;
    case SupportLevel::ValidUnvalidated:
        return 1;
    case SupportLevel::NotImplemented:
        return 2;
    case SupportLevel::Invalid:
        return 3;
    }
    return 3;
}

SupportAnnotation CombineAnnotations(const SupportAnnotation& current, const SupportAnnotation& candidate) {
    if (SeverityRank(candidate.level) > SeverityRank(current.level)) {
        return candidate;
    }
    if (SeverityRank(candidate.level) == SeverityRank(current.level) && current.reason.empty()) {
        return candidate;
    }
    return current;
}

template <typename MapT, typename KeyT>
SupportAnnotation LookupAnnotation(const MapT& map, const KeyT& key, std::string_view dimension_name) {
    const auto it = map.find(key);
    if (it == map.end()) {
        SupportAnnotation missing;
        missing.level = SupportLevel::Invalid;
        missing.reason = std::string("Missing capability annotation for ") + std::string(dimension_name);
        return missing;
    }
    return it->second;
}

SupportAnnotation BaseContainerVideoAudioAnnotation(Container c, VideoCodec v, AudioCodec a) {
    if (c == Container::WebM) {
        if (a == AudioCodec::AacMf) {
            return {SupportLevel::Invalid, "WebM does not support AAC in ExoSnap's product matrix."};
        }
        if (a == AudioCodec::Pcm) {
            return {SupportLevel::Invalid, "WebM + PCM is not supported in the current product matrix."};
        }
        if (v == VideoCodec::H264Nvenc || v == VideoCodec::HevcNvenc) {
            return {SupportLevel::Invalid, "WebM supports AV1 in ExoSnap's product matrix; H.264/HEVC are invalid."};
        }
        if (v == VideoCodec::Av1Nvenc && a == AudioCodec::Opus) {
            return {SupportLevel::Available, "Primary validated WebM path: AV1 NVENC + Opus."};
        }
        return {SupportLevel::Invalid, "This WebM combination is not in the supported product matrix."};
    }

    if (c == Container::Mp4) {
        if (a == AudioCodec::Opus) {
            return {SupportLevel::Invalid, "MP4 does not support Opus; select AAC for MP4 recordings."};
        }
        if (a == AudioCodec::Pcm) {
            return {SupportLevel::Invalid, "MP4 + PCM is not supported."};
        }
        if (a == AudioCodec::AacMf) {
            if (v == VideoCodec::H264Nvenc) {
                return {SupportLevel::NotImplemented,
                        "MP4 + H.264 + AAC is planned; the IMFSinkWriter muxer and Annex-B→AVCC "
                        "conversion are not yet implemented."};
            }
            if (v == VideoCodec::HevcNvenc) {
                return {SupportLevel::NotImplemented, "MP4 + HEVC + AAC is not implemented; implement H.264 first."};
            }
            if (v == VideoCodec::Av1Nvenc) {
                return {SupportLevel::NotImplemented, "AV1-in-MP4 is deferred; use WebM for AV1 recordings."};
            }
        }
        return {SupportLevel::Invalid, "This MP4 combination is not in the supported product matrix."};
    }

    if (c == Container::Matroska) {
        if (v == VideoCodec::Av1Nvenc && a == AudioCodec::AacMf) {
            return {SupportLevel::Available, "Validated M3.2 path."};
        }
        if (v == VideoCodec::Av1Nvenc && a == AudioCodec::Opus) {
            return {SupportLevel::Available, "Validated M4 Opus path (MKV + AV1 NVENC + Opus)."};
        }
        if (v == VideoCodec::Av1Nvenc && a == AudioCodec::Pcm) {
            return {SupportLevel::NotImplemented, "MKV + AV1 + PCM is not implemented."};
        }
        if ((v == VideoCodec::H264Nvenc || v == VideoCodec::HevcNvenc) && a == AudioCodec::AacMf) {
            return {SupportLevel::NotImplemented, "MKV + NVENC H.264/HEVC + AAC is planned but not implemented yet."};
        }
        return {SupportLevel::Invalid, "This MKV combination is not in the supported product matrix."};
    }

    return {SupportLevel::Invalid, "Unknown container in capability matrix."};
}

SupportAnnotation StaticMatrixAnnotation(Container c, VideoCodec v, AudioCodec a, ChromaSubsampling cs, BitDepth bd) {
    const SupportAnnotation base = BaseContainerVideoAudioAnnotation(c, v, a);
    if (base.level == SupportLevel::Invalid) {
        return base;
    }

    if (cs != ChromaSubsampling::Cs420 || bd != BitDepth::Bit8) {
        return {SupportLevel::NotImplemented, "Only 4:2:0 / 8-bit is implemented for current validated paths."};
    }

    return base;
}

} // namespace

size_t ComboKeyHash::operator()(const ComboKey& key) const noexcept {
    size_t seed = 0;
    auto combine = [&seed](size_t value) { seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u); };

    combine(static_cast<size_t>(key.c));
    combine(static_cast<size_t>(key.v));
    combine(static_cast<size_t>(key.a));
    combine(static_cast<size_t>(key.cs));
    combine(static_cast<size_t>(key.bd));
    return seed;
}

SupportAnnotation CapabilitySet::QueryCombo(Container c, VideoCodec v, AudioCodec a, ChromaSubsampling cs,
                                            BitDepth bd) const {
    SupportAnnotation result = StaticMatrixAnnotation(c, v, a, cs, bd);
    result = CombineAnnotations(result, QueryContainer(c));
    result = CombineAnnotations(result, QueryVideoCodec(v));
    result = CombineAnnotations(result, QueryAudioCodec(a));
    result = CombineAnnotations(result, QueryChroma(cs));
    result = CombineAnnotations(result, QueryBitDepth(bd));

    const ComboKey key{c, v, a, cs, bd};
    const auto override_it = combo_overrides.find(key);
    if (override_it != combo_overrides.end()) {
        result = override_it->second;
    }

    return result;
}

SupportAnnotation CapabilitySet::QueryContainer(Container c) const {
    return LookupAnnotation(containers, c, "container");
}

SupportAnnotation CapabilitySet::QueryVideoCodec(VideoCodec v) const {
    return LookupAnnotation(video_codecs, v, "video codec");
}

SupportAnnotation CapabilitySet::QueryAudioCodec(AudioCodec a) const {
    return LookupAnnotation(audio_codecs, a, "audio codec");
}

SupportAnnotation CapabilitySet::QueryChroma(ChromaSubsampling cs) const {
    return LookupAnnotation(chroma_modes, cs, "chroma mode");
}

SupportAnnotation CapabilitySet::QueryBitDepth(BitDepth bd) const {
    return LookupAnnotation(bit_depths, bd, "bit depth");
}

} // namespace exosnap::capability
