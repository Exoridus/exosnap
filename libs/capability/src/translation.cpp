#include <capability/translation.h>

#include <stdexcept>

namespace exosnap::capability {

recorder_core::RecorderConfig ToRecorderCoreConfig(const UserRecorderConfig& config, const CapabilitySet& caps,
                                                   ResolveResult* validation) {
    SettingsResolver resolver(caps);
    ResolveResult resolved = resolver.ValidateConfig(config);

    if (validation != nullptr) {
        *validation = resolved;
    }

    if (!resolved.succeeded) {
        std::string message = "Configuration validation failed.";
        if (!resolved.invalidity.empty()) {
            message += " " + resolved.invalidity.front().message;
        }
        throw std::invalid_argument(message);
    }

    const UserRecorderConfig& final_config = resolved.resolved_config;
    const bool valid_chroma_depth =
        final_config.chroma == ChromaSubsampling::Cs420 && final_config.bit_depth == BitDepth::Bit8;
    const bool is_webm_av1_opus = final_config.container == Container::WebM &&
                                  final_config.video_codec == VideoCodec::Av1Nvenc &&
                                  final_config.audio_codec == AudioCodec::Opus && valid_chroma_depth;
    const bool is_mkv_av1_aac = final_config.container == Container::Matroska &&
                                final_config.video_codec == VideoCodec::Av1Nvenc &&
                                final_config.audio_codec == AudioCodec::AacMf && valid_chroma_depth;
    const bool is_mkv_av1_opus = final_config.container == Container::Matroska &&
                                 final_config.video_codec == VideoCodec::Av1Nvenc &&
                                 final_config.audio_codec == AudioCodec::Opus && valid_chroma_depth;
    const bool is_mkv_h264_aac = final_config.container == Container::Matroska &&
                                 final_config.video_codec == VideoCodec::H264Nvenc &&
                                 final_config.audio_codec == AudioCodec::AacMf && valid_chroma_depth;
    const bool is_mp4_h264_aac = final_config.container == Container::Mp4 &&
                                 final_config.video_codec == VideoCodec::H264Nvenc &&
                                 final_config.audio_codec == AudioCodec::AacMf && valid_chroma_depth;

    if (!is_webm_av1_opus && !is_mkv_av1_aac && !is_mkv_av1_opus && !is_mkv_h264_aac && !is_mp4_h264_aac) {
        ResolveResult failure = resolved;
        failure.succeeded = false;
        failure.invalidity.push_back(InvalidReason{
            "translation",
            "Only WebM+AV1+Opus, Matroska+AV1+(AAC|Opus), Matroska+H264+AAC, or MP4+H264+AAC + 4:2:0 + 8-bit "
            "can be translated to recorder_core."});
        if (validation != nullptr) {
            *validation = failure;
        }
        throw std::invalid_argument(failure.invalidity.back().message);
    }

    recorder_core::RecorderConfig core_config;
    core_config.chroma = recorder_core::ChromaSubsampling::Cs420;
    core_config.bit_depth = recorder_core::BitDepth::Bit8;
    core_config.frame_rate_num = final_config.frame_rate_num;
    core_config.frame_rate_den = final_config.frame_rate_den;

    if (is_webm_av1_opus) {
        core_config.container = recorder_core::Container::WebM;
        core_config.video_codec = recorder_core::VideoCodec::Av1Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Opus;
    } else if (is_mkv_av1_opus) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::Av1Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Opus;
    } else if (is_mkv_h264_aac) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::H264Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::AacMf;
    } else if (is_mp4_h264_aac) {
        core_config.container = recorder_core::Container::Mp4;
        core_config.video_codec = recorder_core::VideoCodec::H264Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::AacMf;
    } else {
        // is_mkv_av1_aac
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::Av1Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::AacMf;
    }

    return core_config;
}

} // namespace exosnap::capability
