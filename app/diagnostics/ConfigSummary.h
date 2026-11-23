#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include <capability/audio_ui_state.h>
#include <capability/user_config.h>

namespace exosnap::diagnostics {

// The string-based diagnostic controller contract is UTF-8, not the
// Windows active ANSI codepage. Convert native filesystem paths explicitly.
[[nodiscard]] std::string PathAsUtf8(const std::filesystem::path& path);

struct ConfigEntry {
    std::string label;
    std::string value;
};

struct ConfigSummary {
    std::vector<ConfigEntry> entries;
    std::string effective_output_path;
    std::string settings_file_path;

    static ConfigSummary FromCurrentSettings(const OutputSettingsModel& output, const VideoSettingsModel& video,
                                             const capability::AudioUiState& audio,
                                             const std::filesystem::path& settings_path,
                                             const std::string& profile_name, const std::string& hotkeys_summary);
};

capability::UserRecorderConfig UserConfigFromSettings(const OutputSettingsModel& output,
                                                      const VideoSettingsModel& video);

} // namespace exosnap::diagnostics
