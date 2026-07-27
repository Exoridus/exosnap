#include <capability/codec_selection.h>

#include <capability/container_compat_registry.h>
#include <capability/support_level.h>

#include <array>

namespace exosnap::capability {

std::optional<VideoCodec> BestAvailableVideoCodec(const CapabilitySet& caps, Container container) noexcept {
    // Preference order: best quality/efficiency first.
    static constexpr std::array<VideoCodec, 3> kPreference = {VideoCodec::Av1, VideoCodec::Hevc, VideoCodec::H264};

    const AudioCodec audio = ContainerCompatRegistry::PreferredAudioCodec(container);

    for (const VideoCodec codec : kPreference) {
        // (a) GPU-supported per the (static + runtime-probed) CapabilitySet.
        if (!IsSelectable(caps.QueryVideoCodec(codec))) {
            continue;
        }
        // (b) container-valid: anything that is not a hard Prohibited block.
        const ContainerCompatEntry entry = ContainerCompatRegistry::Query(container, codec, audio);
        if (IsContainerCompatProhibited(entry.level)) {
            continue;
        }
        return codec;
    }
    return std::nullopt;
}

std::string_view VisibleVideoCodecLabel(VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::Av1:
        return "AV1";
    case VideoCodec::Hevc:
        return "HEVC";
    case VideoCodec::H264:
        return "H.264";
    }
    return "AV1";
}

} // namespace exosnap::capability
