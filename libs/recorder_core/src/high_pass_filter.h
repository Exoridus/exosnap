#pragma once

#include <array>
#include <cstdint>

namespace recorder_core {

// ---------------------------------------------------------------------------
// HighPassFilter (Audio v2 — 0.6.0)
// ---------------------------------------------------------------------------
//
// 2nd-order Butterworth high-pass biquad for interleaved Float32 audio, applied
// per channel. Removes low-frequency rumble from microphone input (desk thumps,
// HVAC hum, plosives) that otherwise wastes bitrate and muddies the recording.
//
// Coefficients are the RBJ audio-EQ cookbook high-pass with Q = 1/sqrt(2)
// (Butterworth, maximally flat passband). The biquad runs in Transposed Direct
// Form II, which keeps per-channel state to two registers and is numerically
// well-behaved at audio sample rates.
//
// State (the two TDF-II registers per channel) is carried across Process()
// calls, so a stream can be processed in blocks. Reset() clears it. The class is
// not thread-safe; process one stream from one thread.
class HighPassFilter {
  public:
    // Maximum interleaved channel count we keep state for. WASAPI mic capture is
    // mono or stereo; 8 covers any reasonable multichannel endpoint.
    static constexpr uint32_t kMaxChannels = 8;

    struct Config {
        // Cutoff (−3 dB) frequency in Hz. Clamped to (0, sample_rate / 2).
        float cutoff_hz = 80.0f;
        uint32_t sample_rate = 48000;
        uint32_t channels = 2;
    };

    HighPassFilter() = default;
    explicit HighPassFilter(const Config& cfg);

    // (Re)configure. Sanitizes degenerate inputs (channels in [1, kMaxChannels],
    // sample_rate >= 1, cutoff_hz clamped to (0, sample_rate / 2)) and recomputes
    // the biquad coefficients. Does not clear the per-channel state.
    void Configure(const Config& cfg);

    // Clear the per-channel TDF-II state registers.
    void Reset() noexcept;

    // Process `frames` interleaved samples in place. The buffer must hold
    // frames * Config.channels floats.
    void Process(float* interleaved, uint32_t frames) noexcept;

    [[nodiscard]] const Config& GetConfig() const noexcept {
        return cfg_;
    }

  private:
    void RecomputeCoeffs() noexcept;

    Config cfg_{};

    // RBJ biquad coefficients (already normalized by a0).
    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;

    // Transposed Direct Form II state, two registers per channel.
    std::array<float, kMaxChannels> z1_{};
    std::array<float, kMaxChannels> z2_{};
};

} // namespace recorder_core
