#pragma once

#include <cstdint>
#include <vector>

namespace recorder_core {

enum class AudioSourceKind {
    App,
    Mic,
    Sys,
    SystemOutput, // Full system output via WasapiLoopbackSrc; no PID required.
};

struct AudioSourceRow {
    AudioSourceKind kind = AudioSourceKind::App;
    bool enabled = true;
    bool merge_with_above = false;
};

struct ResolvedAudioTrack {
    std::vector<AudioSourceKind> sources;
    uint32_t track_index = 0;
};

struct AudioTrackPlan {
    std::vector<ResolvedAudioTrack> tracks;
};

[[nodiscard]] AudioTrackPlan ResolveAudioTracks(const std::vector<AudioSourceRow>& rows);

} // namespace recorder_core
