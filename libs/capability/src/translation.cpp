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
    // Chroma is fixed at 4:2:0. Bit depth is 8-bit universally; 10-bit (HEVC Main10 /
    // AV1 10-bit P010, SDR BT.709 — ADR 0032) is valid only for HEVC and AV1, never
    // H.264. The per-combo flags below combine this with the codec, so a 10-bit H.264
    // request falls through to the rejection path.
    const bool is_hevc = final_config.video_codec == VideoCodec::HevcNvenc;
    const bool is_av1 = final_config.video_codec == VideoCodec::Av1Nvenc;
    const bool bit_depth_ok =
        final_config.bit_depth == BitDepth::Bit8 || (final_config.bit_depth == BitDepth::Bit10 && (is_hevc || is_av1));
    const bool valid_chroma_depth = final_config.chroma == ChromaSubsampling::Cs420 && bit_depth_ok;
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
    const bool is_mkv_av1_pcm = final_config.container == Container::Matroska &&
                                final_config.video_codec == VideoCodec::Av1Nvenc &&
                                final_config.audio_codec == AudioCodec::Pcm && valid_chroma_depth;
    const bool is_mkv_h264_pcm = final_config.container == Container::Matroska &&
                                 final_config.video_codec == VideoCodec::H264Nvenc &&
                                 final_config.audio_codec == AudioCodec::Pcm && valid_chroma_depth;
    const bool is_mkv_av1_flac = final_config.container == Container::Matroska &&
                                 final_config.video_codec == VideoCodec::Av1Nvenc &&
                                 final_config.audio_codec == AudioCodec::Flac && valid_chroma_depth;
    const bool is_mkv_h264_flac = final_config.container == Container::Matroska &&
                                  final_config.video_codec == VideoCodec::H264Nvenc &&
                                  final_config.audio_codec == AudioCodec::Flac && valid_chroma_depth;
    const bool is_mkv_hevc_aac = final_config.container == Container::Matroska &&
                                 final_config.video_codec == VideoCodec::HevcNvenc &&
                                 final_config.audio_codec == AudioCodec::AacMf && valid_chroma_depth;
    const bool is_mkv_hevc_opus = final_config.container == Container::Matroska &&
                                  final_config.video_codec == VideoCodec::HevcNvenc &&
                                  final_config.audio_codec == AudioCodec::Opus && valid_chroma_depth;
    const bool is_mkv_hevc_pcm = final_config.container == Container::Matroska &&
                                 final_config.video_codec == VideoCodec::HevcNvenc &&
                                 final_config.audio_codec == AudioCodec::Pcm && valid_chroma_depth;
    const bool is_mkv_hevc_flac = final_config.container == Container::Matroska &&
                                  final_config.video_codec == VideoCodec::HevcNvenc &&
                                  final_config.audio_codec == AudioCodec::Flac && valid_chroma_depth;
    const bool is_mp4_h264_aac = final_config.container == Container::Mp4 &&
                                 final_config.video_codec == VideoCodec::H264Nvenc &&
                                 final_config.audio_codec == AudioCodec::AacMf && valid_chroma_depth;
    const bool is_mp4_hevc_aac = final_config.container == Container::Mp4 &&
                                 final_config.video_codec == VideoCodec::HevcNvenc &&
                                 final_config.audio_codec == AudioCodec::AacMf && valid_chroma_depth;

    if (!is_webm_av1_opus && !is_mkv_av1_aac && !is_mkv_av1_opus && !is_mkv_h264_aac && !is_mkv_av1_pcm &&
        !is_mkv_h264_pcm && !is_mkv_av1_flac && !is_mkv_h264_flac && !is_mkv_hevc_aac && !is_mkv_hevc_opus &&
        !is_mkv_hevc_pcm && !is_mkv_hevc_flac && !is_mp4_h264_aac && !is_mp4_hevc_aac) {
        ResolveResult failure = resolved;
        failure.succeeded = false;
        failure.invalidity.push_back(InvalidReason{
            "translation", "Only WebM+AV1+Opus, Matroska+AV1+(AAC|Opus|PCM|FLAC), Matroska+H264+(AAC|PCM|FLAC), "
                           "Matroska+HEVC+(AAC|Opus|PCM|FLAC), or MP4+(H264|HEVC)+AAC + 4:2:0 + 8-bit "
                           "(or 10-bit with HEVC/AV1) can be translated to recorder_core."});
        if (validation != nullptr) {
            *validation = failure;
        }
        throw std::invalid_argument(failure.invalidity.back().message);
    }

    recorder_core::RecorderConfig core_config;
    core_config.chroma = recorder_core::ChromaSubsampling::Cs420;
    core_config.bit_depth =
        (final_config.bit_depth == BitDepth::Bit10) ? recorder_core::BitDepth::Bit10 : recorder_core::BitDepth::Bit8;
    // Colour range (0.7.0): always valid for every codec/container, so it is NOT
    // part of the combo allow-list above — just carry it through to the engine.
    // Primaries/transfer/matrix stay BT.709 SDR (the ColorMetadata defaults).
    core_config.color.range = (final_config.color_range == ColorRange::Limited) ? recorder_core::ColorRange::Limited
                                                                                : recorder_core::ColorRange::Full;
    core_config.frame_rate_num = final_config.frame_rate_num;
    core_config.frame_rate_den = final_config.frame_rate_den;
    core_config.output_width = final_config.output_width;
    core_config.output_height = final_config.output_height;

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
    } else if (is_mkv_av1_pcm) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::Av1Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Pcm;
    } else if (is_mkv_h264_pcm) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::H264Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Pcm;
    } else if (is_mkv_av1_flac) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::Av1Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Flac;
    } else if (is_mkv_h264_flac) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::H264Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Flac;
    } else if (is_mkv_hevc_aac) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::HevcNvenc;
        core_config.audio_codec = recorder_core::AudioCodec::AacMf;
    } else if (is_mkv_hevc_opus) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::HevcNvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Opus;
    } else if (is_mkv_hevc_pcm) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::HevcNvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Pcm;
    } else if (is_mkv_hevc_flac) {
        core_config.container = recorder_core::Container::Matroska;
        core_config.video_codec = recorder_core::VideoCodec::HevcNvenc;
        core_config.audio_codec = recorder_core::AudioCodec::Flac;
    } else if (is_mp4_h264_aac) {
        core_config.container = recorder_core::Container::Mp4;
        core_config.video_codec = recorder_core::VideoCodec::H264Nvenc;
        core_config.audio_codec = recorder_core::AudioCodec::AacMf;
    } else if (is_mp4_hevc_aac) {
        core_config.container = recorder_core::Container::Mp4;
        core_config.video_codec = recorder_core::VideoCodec::HevcNvenc;
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
