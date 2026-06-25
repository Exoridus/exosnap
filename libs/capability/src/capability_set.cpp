#include <capability/capability_set.h>

#include <capability/container_compat_registry.h>

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

// Maps a ContainerCompatLevel from the registry to a SupportLevel for the
// CapabilitySet query layer.
//
// Mapping rationale (ADR 0010):
//   Recommended  → Available        (vetted; fully selectable)
//   Allowed      → ValidUnvalidated (selectable with caveat; warning emitted)
//   Experimental → NotImplemented   (not user-selectable until validated)
//   Fallback     → NotImplemented   (not user-selectable)
//   Prohibited   → Invalid          (hard block; must not appear in UI)
SupportAnnotation RegistryEntryToSupportAnnotation(const ContainerCompatEntry& entry) {
    switch (entry.level) {
    case ContainerCompatLevel::Recommended:
        return {SupportLevel::Available, std::string(entry.reason)};
    case ContainerCompatLevel::Allowed:
        return {SupportLevel::ValidUnvalidated, std::string(entry.reason)};
    case ContainerCompatLevel::Experimental:
        return {SupportLevel::NotImplemented, std::string(entry.reason)};
    case ContainerCompatLevel::Fallback:
        return {SupportLevel::NotImplemented, std::string(entry.reason)};
    case ContainerCompatLevel::Prohibited:
        return {SupportLevel::Invalid, std::string(entry.reason)};
    }
    return {SupportLevel::Invalid, "Unknown compatibility level."};
}

SupportAnnotation BaseContainerVideoAudioAnnotation(Container c, VideoCodec v, AudioCodec a) {
    return RegistryEntryToSupportAnnotation(ContainerCompatRegistry::Query(c, v, a));
}

SupportAnnotation StaticMatrixAnnotation(Container c, VideoCodec v, AudioCodec a, ChromaSubsampling cs, BitDepth bd) {
    const SupportAnnotation base = BaseContainerVideoAudioAnnotation(c, v, a);
    if (base.level == SupportLevel::Invalid) {
        return base;
    }

    if (cs != ChromaSubsampling::Cs420) {
        return {SupportLevel::NotImplemented, "Only 4:2:0 chroma is implemented for current validated paths."};
    }

    // 8-bit is universal. 10-bit (HEVC Main10 / AV1 10-bit, P010, SDR BT.709 — 0.7.0 S5)
    // is implemented only for the NVENC HEVC and AV1 codecs; H.264 stays 8-bit only.
    if (bd == BitDepth::Bit10 && v != VideoCodec::HevcNvenc && v != VideoCodec::Av1Nvenc) {
        return {SupportLevel::NotImplemented, "10-bit requires the HEVC or AV1 codec; H.264 is 8-bit only."};
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

SupportAnnotation CapabilitySet::QueryRateControlMode(recorder_core::RateControlMode mode) const {
    // Static capability declaration for NVENC (ADR 0009).
    // CQ / VBR / CBR are implemented and available.
    // Lossless is not yet implemented for any encoder — hidden in UI per ADR 0009.
    switch (mode) {
    case recorder_core::RateControlMode::ConstantQuality:
        return {SupportLevel::Available, "NVENC CQP — quality-target, encoder chooses bitrate."};
    case recorder_core::RateControlMode::VariableBitrate:
        return {SupportLevel::Available, "NVENC VBR — encoder targets a bitrate, quality varies."};
    case recorder_core::RateControlMode::ConstantBitrate:
        return {SupportLevel::Available, "NVENC CBR — strict bitrate, quality managed by encoder."};
    case recorder_core::RateControlMode::Lossless:
        return {SupportLevel::NotImplemented, "Lossless encoding is not yet implemented. Hidden in UI."};
    }
    return {SupportLevel::Invalid, "Unknown rate-control mode."};
}

} // namespace exosnap::capability
