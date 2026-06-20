#pragma once

#include <cstdint>

namespace recorder_core {

// ---------------------------------------------------------------------------
// NoiseGate (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// Stereo-linked downward noise gate for interleaved Float32 audio. The second
// stage of the MicDspAudioSrc chain (after the high-pass filter): when the mic
// level falls below a threshold it attenuates the signal toward silence (keyboard
// clatter, fan noise, room tone between phrases); when the level rises above the
// threshold it opens and passes the signal through at unity gain.
//
// Detection is stereo-linked — the gate decision uses the loudest channel of the
// frame and one shared gain is applied to every channel — so the stereo image is
// preserved and channels open/close together (no L/R flutter).
//
// Anti-chatter: once the gate opens it stays open for `hold_ms` after the level
// last exceeded the threshold (a hold timer), and the applied gain is smoothed
// with a one-pole envelope — a fast `attack` coefficient when opening and a slow
// `release` coefficient when closing. This keeps the gate from rapidly toggling
// around the threshold.
//
// State (the smoothed gain and the hold timer) carries across Process() calls so
// a stream can be processed in blocks. Reset() clears it. The class is not
// thread-safe; process one stream from one thread (the audio thread).
class NoiseGate {
  public:
    // Maximum interleaved channel count we accept. The gate keeps no per-channel
    // state (one shared gain), but the count is clamped to a sane bound to match
    // the rest of the mic-DSP chain. WASAPI mic capture is mono or stereo; 8
    // covers any reasonable multichannel endpoint.
    static constexpr uint32_t kMaxChannels = 8;

    struct Config {
        // Level below which the gate closes (attenuates), in dBFS. Above it the
        // gate opens. Reset to -45 dB when non-finite.
        float threshold_db = -45.0f;
        // Gain ramp-up time (ms) when the gate opens. Smaller = snappier onset.
        float attack_ms = 2.0f;
        // How long the gate stays open after the level last exceeded the
        // threshold (ms). Prevents the tail of speech from being chopped.
        float hold_ms = 120.0f;
        // Gain ramp-down time (ms) when the gate closes. Larger = gentler fade.
        float release_ms = 150.0f;
        uint32_t sample_rate = 48000;
        uint32_t channels = 2;
    };

    NoiseGate() = default;
    explicit NoiseGate(const Config& cfg);

    // (Re)configure. Sanitizes degenerate inputs (channels in [1, kMaxChannels],
    // sample_rate >= 1, threshold reset to -45 dB when non-finite), recomputes the
    // attack/release coefficients and the linear threshold. Does not clear state.
    void Configure(const Config& cfg);

    // Clear the smoothed gain (back to closed) and the hold timer.
    void Reset() noexcept;

    // Process `frames` interleaved samples in place. The buffer must hold
    // frames * Config.channels floats.
    void Process(float* interleaved, uint32_t frames) noexcept;

    // Current smoothed gate gain (1.0 == fully open, 0.0 == fully closed).
    // Exposed for tests/metering.
    [[nodiscard]] float CurrentGain() const noexcept {
        return gain_;
    }

    [[nodiscard]] const Config& GetConfig() const noexcept {
        return cfg_;
    }

  private:
    void RecomputeCoeffs() noexcept;

    Config cfg_{};
    float threshold_linear_ = 0.0f; // pow(10, threshold_db / 20)
    float attack_coeff_ = 0.0f;     // one-pole smoothing toward the open target
    float release_coeff_ = 0.0f;    // one-pole smoothing toward the closed target
    float gain_ = 0.0f;             // current smoothed gate gain (starts closed)
    uint32_t hold_frames_remaining_ = 0;
};

} // namespace recorder_core
