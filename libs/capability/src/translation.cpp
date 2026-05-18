#include <capability/translation.h>

#include <stdexcept>

namespace exosnap::capability {

recorder_core::RecorderConfig ToRecorderCoreConfig(
    const UserRecorderConfig& config,
    const CapabilitySet& caps,
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
    const bool is_m32_combo =
        final_config.container == Container::Matroska
        && final_config.video_codec == VideoCodec::Av1Nvenc
        && final_config.audio_codec == AudioCodec::AacMf
        && final_config.chroma == ChromaSubsampling::Cs420
        && final_config.bit_depth == BitDepth::Bit8;

    if (!is_m32_combo) {
        ResolveResult failure = resolved;
        failure.succeeded = false;
        failure.invalidity.push_back(InvalidReason{
            "translation",
            "Only Matroska + AV1 NVENC + AAC-MF + 4:2:0 + 8-bit can be translated to recorder_core in M3.3A."
        });
        if (validation != nullptr) {
            *validation = failure;
        }
        throw std::invalid_argument(failure.invalidity.back().message);
    }

    recorder_core::RecorderConfig core_config;
    core_config.container     = recorder_core::Container::Matroska;
    core_config.video_codec   = recorder_core::VideoCodec::Av1Nvenc;
    core_config.audio_codec   = recorder_core::AudioCodec::AacMf;
    core_config.chroma        = recorder_core::ChromaSubsampling::Cs420;
    core_config.bit_depth     = recorder_core::BitDepth::Bit8;
    core_config.frame_rate_num = final_config.frame_rate_num;
    core_config.frame_rate_den = final_config.frame_rate_den;

    return core_config;
}

} // namespace exosnap::capability

