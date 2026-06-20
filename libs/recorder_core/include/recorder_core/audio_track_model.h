#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace recorder_core {

enum class AudioSourceKind {
    App,
    Mic,
    Sys,
    SystemOutput, // Full system output via WasapiLoopbackSrc; no PID required.
};

// ---------------------------------------------------------------------------
// Per-row gain + mute (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------

// Minimum / maximum gain in dB for per-row gain control.
inline constexpr float kMinGainDb = -60.0f;
inline constexpr float kMaxGainDb = +24.0f;

// Convert a dB gain value and muted flag to a linear multiplier.
// Returns 0.0f when muted; otherwise pow(10, gain_db/20).
// Input is expected to be in [kMinGainDb, kMaxGainDb]; clamping is the
// caller's responsibility (SanitizePresetConfig does it for persisted state).
[[nodiscard]] inline float GainDbToLinear(float gain_db, bool muted) noexcept {
    if (muted) {
        return 0.0f;
    }
    return std::powf(10.0f, gain_db / 20.0f);
}

struct AudioSourceRow {
    AudioSourceKind kind = AudioSourceKind::App;
    bool enabled = true;
    bool merge_with_above = false;

    // Per-row gain in dB (0 dB = unity). Range [-60, +24].
    float gain_db = 0.0f;
    // When true, this source contributes silence (GainDbToLinear returns 0).
    bool muted = false;
};

struct ResolvedAudioTrack {
    std::vector<AudioSourceKind> sources;
    // Per-source linear gain multipliers, parallel to `sources`.
    // Populated by ResolveAudioTracks from each row's gain_db and muted flag.
    // Length always equals sources.size().
    std::vector<float> source_gain_linear;
    uint32_t track_index = 0;
};

struct AudioTrackPlan {
    std::vector<ResolvedAudioTrack> tracks;
};

[[nodiscard]] AudioTrackPlan ResolveAudioTracks(const std::vector<AudioSourceRow>& rows);

} // namespace recorder_core
