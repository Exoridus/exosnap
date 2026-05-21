#pragma once

#include <capability/config_types.h>

#include <filesystem>
#include <string>

namespace exosnap {

struct OutputSettingsModel {
    std::filesystem::path output_folder;
    std::wstring naming_pattern = L"exosnap_{date}_{time}";
    capability::Container container = capability::Container::Matroska;
    capability::AudioCodec audio_codec = capability::AudioCodec::Opus;

    static OutputSettingsModel Defaults();
};

} // namespace exosnap
