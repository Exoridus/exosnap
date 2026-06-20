#pragma once

#include <cstdint>

namespace recorder_core {

// ---------------------------------------------------------------------------
// AutomaticGainControl (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// Stereo-linked automatic gain control (AGC) for interleaved Float32 audio. The
// third stage of the MicDspAudioSrc chain (after the high-pass filter and the
// noise gate): it tracks the mic level over time and applies a slowly-varying
// makeup gain toward a target level so a quiet talker is boosted and a loud one
// is attenuated, keeping the recorded voice at a consistent loudness.
//
// Detection is stereo-linked — a smoothed envelope is derived from the loudest
// channel of each frame (one-pole envelope follower) and one shared makeup gain
// is applied to every channel — so the stereo image is preserved and channels
// track together (no L/R drift).
//
// Noise-floor freeze: when the envelope falls below `noise_floor_db` the gain is
// frozen (held at its current value). This is the key safety property — the AGC
// must not crank its gain up while the input is near-silence/noise, which would
// otherwise produce runaway gain that then slams the level on the next loud
// sample (and amplifies whatever residual noise survived the gate).
//
// The makeup gain is smoothed with a one-pole envelope — a fast `attack`
// coefficient when the gain rises (boosting), a slow `release` coefficient when
// it falls (attenuating). State (the smoothed gain and the level envelope)
// carries across Process() calls so a stream can be processed in blocks.
// Reset() clears it. The class is not thread-safe; process one stream from one
// thread (the audio thread).
class AutomaticGainControl {
  public:
    // Maximum interleaved channel count we accept. AGC keeps no per-channel
    // state (one shared gain + one shared envelope), but the count is clamped to
    // a sane bound to match the rest of the mic-DSP chain. WASAPI mic capture is
    // mono or stereo; 8 covers any reasonable multichannel endpoint.
    static constexpr uint32_t kMaxChannels = 8;

    struct Config {
        // Loudness the AGC drives the signal toward, in dBFS. Reset to -18 dB
        // when non-finite.
        float target_db = -18.0f;
        // Maximum makeup gain magnitude, in dB (>= 0). Bounds both boost
        // (+max_gain_db) and attenuation (-max_gain_db). Reset to 30 dB when
        // non-finite; a negative value is made non-negative.
        float max_gain_db = 30.0f;
        // Gain ramp-up time (ms) when boosting toward the target. Larger =
        // gentler. Reset to 50 ms when non-finite.
        float attack_ms = 50.0f;
        // Gain ramp-down time (ms) when attenuating toward the target. Larger =
        // gentler. Reset to 400 ms when non-finite.
        float release_ms = 400.0f;
        // Level below which the gain is frozen (no boost into silence/noise), in
        // dBFS. Reset to -55 dB when non-finite.
        float noise_floor_db = -55.0f;
        uint32_t sample_rate = 48000;
        uint32_t channels = 2;
    };

    AutomaticGainControl() = default;
    explicit AutomaticGainControl(const Config& cfg);

    // (Re)configure. Sanitizes degenerate inputs (channels in [1, kMaxChannels],
    // sample_rate >= 1, all *_db reset to defaults when non-finite, max_gain_db
    // made non-negative), recomputes the linear targets/floor and the
    // attack/release coefficients. Does not clear state.
    void Configure(const Config& cfg);

    // Reset the makeup gain to unity and clear the level envelope.
    void Reset() noexcept;

    // Process `frames` interleaved samples in place. The buffer must hold
    // frames * Config.channels floats.
    void Process(float* interleaved, uint32_t frames) noexcept;

    // Current smoothed makeup gain (1.0 == unity). Exposed for tests/metering.
    [[nodiscard]] float CurrentGain() const noexcept {
        return gain_;
    }

    [[nodiscard]] const Config& GetConfig() const noexcept {
        return cfg_;
    }

  private:
    void RecomputeCoeffs() noexcept;

    Config cfg_{};
    float target_linear_ = 0.0f;      // pow(10, target_db / 20)
    float noise_floor_linear_ = 0.0f; // pow(10, noise_floor_db / 20)
    float max_gain_linear_ = 1.0f;    // pow(10, max_gain_db / 20)
    float min_gain_linear_ = 1.0f;    // pow(10, -max_gain_db / 20)
    float env_coeff_ = 0.0f;          // one-pole smoothing for the level envelope
    float attack_coeff_ = 0.0f;       // one-pole smoothing when the gain rises
    float release_coeff_ = 0.0f;      // one-pole smoothing when the gain falls
    float gain_ = 1.0f;               // current smoothed makeup gain (starts at unity)
    float env_ = 0.0f;                // current smoothed input-level envelope
};

} // namespace recorder_core
