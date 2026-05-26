#include "ConfigSummary.h"

#include <capability/config_types.h>

namespace exosnap::diagnostics {

namespace {

std::string ContainerName(capability::Container c) {
    switch (c) {
    case capability::Container::Matroska:
        return "MKV";
    case capability::Container::Mp4:
        return "MP4";
    case capability::Container::WebM:
        return "WebM";
    }
    return "Unknown";
}

std::string VideoCodecName(capability::VideoCodec v) {
    switch (v) {
    case capability::VideoCodec::H264Nvenc:
        return "H.264 (NVENC)";
    case capability::VideoCodec::HevcNvenc:
        return "HEVC (NVENC)";
    case capability::VideoCodec::Av1Nvenc:
        return "AV1 (NVENC)";
    }
    return "Unknown";
}

std::string AudioCodecName(capability::AudioCodec a) {
    switch (a) {
    case capability::AudioCodec::Opus:
        return "Opus";
    case capability::AudioCodec::AacMf:
        return "AAC";
    case capability::AudioCodec::Pcm:
        return "PCM";
    }
    return "Unknown";
}

std::string AudioSourceKindName(recorder_core::AudioSourceKind k) {
    switch (k) {
    case recorder_core::AudioSourceKind::App:
        return "APP";
    case recorder_core::AudioSourceKind::Mic:
        return "MIC";
    case recorder_core::AudioSourceKind::Sys:
        return "SYS";
    case recorder_core::AudioSourceKind::SystemOutput:
        return "SYS";
    }
    return "Unknown";
}

std::string MicChannelModeName(recorder_core::MicChannelMode m) {
    switch (m) {
    case recorder_core::MicChannelMode::Auto:
        return "Auto";
    case recorder_core::MicChannelMode::PreserveStereo:
        return "Preserve Stereo";
    case recorder_core::MicChannelMode::MonoMix:
        return "Mono Mix";
    case recorder_core::MicChannelMode::LeftToStereo:
        return "Left to Stereo";
    case recorder_core::MicChannelMode::RightToStereo:
        return "Right to Stereo";
    }
    return "Auto";
}

std::string NvencQualityName(recorder_core::NvencQualityPreset q) {
    switch (q) {
    case recorder_core::NvencQualityPreset::High:
        return "High";
    case recorder_core::NvencQualityPreset::Balanced:
        return "Balanced";
    case recorder_core::NvencQualityPreset::Small:
        return "Small";
    }
    return "Balanced";
}

} // namespace

capability::UserRecorderConfig UserConfigFromSettings(const OutputSettingsModel& output,
                                                      const VideoSettingsModel& video) {
    (void)video;
    capability::UserRecorderConfig config;
    config.container = output.container;
    config.video_codec = output.video_codec;
    config.audio_codec = output.audio_codec;
    config.chroma = capability::ChromaSubsampling::Cs420;
    config.bit_depth = capability::BitDepth::Bit8;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    return config;
}

ConfigSummary ConfigSummary::FromCurrentSettings(const OutputSettingsModel& output, const VideoSettingsModel& video,
                                                 const capability::AudioUiState& audio,
                                                 const std::filesystem::path& settings_path,
                                                 const std::string& profile_name, const std::string& hotkeys_summary) {

    ConfigSummary summary;
    summary.settings_file_path = settings_path.string();

    summary.entries.push_back({"Recording Profile", profile_name});
    summary.entries.push_back(
        {"Target", audio.target_kind == capability::CaptureTargetKind::Display ? "Display" : "Window"});

    summary.entries.push_back({"Container", ContainerName(output.container)});
    summary.entries.push_back({"Video Codec", VideoCodecName(output.video_codec)});
    summary.entries.push_back({"Audio Codec", AudioCodecName(output.audio_codec)});
    summary.entries.push_back({"FPS", "60"});
    summary.entries.push_back({"CFR/VFR", video.cfr ? "CFR" : "VFR"});
    summary.entries.push_back({"Quality", NvencQualityName(video.quality)});
    summary.entries.push_back({"Capture Cursor", video.capture_cursor ? "Yes" : "No"});

    // Audio routing
    std::string audio_routing;
    for (size_t i = 0; i < audio.source_rows.size(); ++i) {
        if (i > 0)
            audio_routing += ", ";
        const auto& row = audio.source_rows[i];
        audio_routing += AudioSourceKindName(row.kind);
        if (!row.enabled)
            audio_routing += " [OFF]";
        if (row.merge_with_above)
            audio_routing += " [MERGED]";
    }
    if (audio_routing.empty())
        audio_routing = "None";
    summary.entries.push_back({"Audio Routing", audio_routing});

    if (audio.selected_mic_device_id.has_value()) {
        summary.entries.push_back({"Mic Device ID", *audio.selected_mic_device_id});
    } else {
        summary.entries.push_back({"Mic Device ID", "Default"});
    }
    summary.entries.push_back({"Mic Channel Mode", MicChannelModeName(audio.mic_channel_mode)});
    summary.entries.push_back({"Mic Gain", std::to_string(audio.mic_gain_linear) + "x"});

    summary.entries.push_back({"Output Folder", output.output_folder.string()});
    summary.entries.push_back({"Filename Pattern", std::filesystem::path(output.naming_pattern).string()});

    summary.effective_output_path = (output.output_folder / output.naming_pattern).string();
    summary.entries.push_back({"Effective Output Path", summary.effective_output_path});

    summary.entries.push_back({"Hotkeys", hotkeys_summary});

    return summary;
}

} // namespace exosnap::diagnostics
