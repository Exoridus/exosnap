#pragma once

#include "capability/audio_ui_state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::capability {

struct AudioTrackPreview {
    uint32_t track_number = 0; // 1-based for UI display
    std::string source_key;    // "app", "sys", "mic", "system_output"
    std::string display_label; // English fallback label
};

[[nodiscard]] std::vector<AudioTrackPreview> BuildAudioTrackPreview(const AudioPlanResult& result);

} // namespace exosnap::capability
