#pragma once

#include <capability/config_types.h>
#include <recorder_core/output_geometry.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace exosnap {

enum class OutputResolutionMode {
    Native,
    UHD2160,
    QHD1440,
    FHD1080,
    HD720,
    Custom,
};

struct OutputResolutionSettings {
    OutputResolutionMode mode = OutputResolutionMode::Native;
    uint32_t custom_width = 0;
    uint32_t custom_height = 0;
    recorder_core::OutputFitMode fit = recorder_core::OutputFitMode::Contain;
};

struct OutputSettingsModel {
    std::filesystem::path output_folder;
    std::wstring naming_pattern = L"{datetime}_{app}_{title}";
    capability::Container container = capability::Container::Matroska;
    capability::VideoCodec video_codec = capability::VideoCodec::H264Nvenc;
    capability::AudioCodec audio_codec = capability::AudioCodec::AacMf;
    OutputResolutionSettings resolution;

    static OutputSettingsModel Defaults();
};

[[nodiscard]] std::optional<recorder_core::FrameSize> PresetOutputSize(OutputResolutionMode mode) noexcept;
[[nodiscard]] const wchar_t* OutputResolutionModeName(OutputResolutionMode mode) noexcept;
[[nodiscard]] const wchar_t* OutputFitModeName(recorder_core::OutputFitMode mode) noexcept;
[[nodiscard]] std::optional<recorder_core::FrameSize>
ResolveRequestedOutputSize(const OutputResolutionSettings& settings, recorder_core::FrameSize source) noexcept;
void SanitizeOutputResolution(OutputResolutionSettings& settings) noexcept;

} // namespace exosnap
