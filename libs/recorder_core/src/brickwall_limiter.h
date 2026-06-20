#pragma once

#include <cstdint>

namespace recorder_core {

// ---------------------------------------------------------------------------
// BrickwallLimiter (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// Stereo-linked feedforward peak limiter for interleaved Float32 audio.
//
// Replaces naive hard-clipping: instead of chopping the waveform when a peak
// exceeds the ceiling, gain is reduced smoothly (attack/release envelope) so the
// signal stays under the ceiling and the result sounds clean. A final per-sample
// clamp backstops the envelope's attack lag (the first sample of a sudden
// transient can momentarily exceed the ceiling before the envelope reacts),
// which makes the ceiling a true brickwall guarantee without needing lookahead.
//
// Below the ceiling the limiter is transparent: the envelope returns to unity
// (gain 1.0) and samples pass through unchanged.
//
// State (the gain envelope) is carried across Process() calls, so a stream can
// be processed in blocks. Reset() clears it. The class is not thread-safe;
// process one stream from one thread.
class BrickwallLimiter {
  public:
    struct Config {
        // Maximum absolute output amplitude. 1.0 == 0 dBFS. Must be > 0.
        float ceiling_linear = 1.0f;
        // Gain-reduction onset time (ms). Smaller = catches transients faster.
        float attack_ms = 1.0f;
        // Gain recovery time (ms) back toward unity once peaks subside.
        float release_ms = 80.0f;
        uint32_t sample_rate = 48000;
        uint32_t channels = 2;
    };

    BrickwallLimiter() = default;
    explicit BrickwallLimiter(const Config& cfg);

    // (Re)configure. Sanitizes degenerate inputs (channels/sample_rate >= 1,
    // ceiling_linear > 0). Does not reset the gain envelope.
    void Configure(const Config& cfg);

    // Clear the gain envelope back to unity (1.0).
    void Reset() noexcept;

    // Process `frames` interleaved samples in place. The buffer must hold
    // frames * Config.channels floats.
    void Process(float* interleaved, uint32_t frames) noexcept;

    // Current envelope gain (1.0 == no reduction). Exposed for tests/metering.
    [[nodiscard]] float CurrentGain() const noexcept {
        return gain_;
    }

    [[nodiscard]] const Config& GetConfig() const noexcept {
        return cfg_;
    }

  private:
    void RecomputeCoeffs() noexcept;

    Config cfg_{};
    float attack_coeff_ = 0.0f;  // one-pole smoothing toward more reduction
    float release_coeff_ = 0.0f; // one-pole smoothing toward unity
    float gain_ = 1.0f;          // current envelope gain
};

// Convert a ceiling in dBFS (<= 0) to a linear amplitude. dB > 0 is clamped to
// 0 dBFS (1.0); non-finite input falls back to 1.0.
[[nodiscard]] float LimiterCeilingDbToLinear(float ceiling_db) noexcept;

} // namespace recorder_core
