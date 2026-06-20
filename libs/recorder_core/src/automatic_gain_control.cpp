#include "automatic_gain_control.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace recorder_core {

namespace {

// One-pole smoothing coefficient for a given time constant. coeff in [0,1):
// state = target + (state - target) * coeff. A 0 ms time → 0 (instant).
float TimeToCoeff(float ms, uint32_t sample_rate) noexcept {
    if (!(ms > 0.0f) || sample_rate == 0) {
        return 0.0f;
    }
    const float samples = (ms * 0.001f) * static_cast<float>(sample_rate);
    if (!(samples > 0.0f)) {
        return 0.0f;
    }
    return std::exp(-1.0f / samples);
}

// Time constant (ms) for the level envelope follower. Fixed: the envelope tracks
// the program loudness, not transients, so a ~10 ms smoothing window keeps the
// gain decision stable without chasing every sample.
constexpr float kEnvelopeMs = 10.0f;

// Floor for the envelope in the desired-gain divisor so it can never blow up.
constexpr float kTinyLevel = 1e-9f;

} // namespace

AutomaticGainControl::AutomaticGainControl(const Config& cfg) {
    Configure(cfg);
}

void AutomaticGainControl::Configure(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.channels == 0) {
        cfg_.channels = 1;
    }
    if (cfg_.channels > kMaxChannels) {
        cfg_.channels = kMaxChannels;
    }
    if (cfg_.sample_rate == 0) {
        cfg_.sample_rate = 48000;
    }
    if (!std::isfinite(cfg_.target_db)) {
        cfg_.target_db = -18.0f;
    }
    if (!std::isfinite(cfg_.max_gain_db)) {
        cfg_.max_gain_db = 30.0f;
    }
    if (cfg_.max_gain_db < 0.0f) {
        cfg_.max_gain_db = -cfg_.max_gain_db;
    }
    if (!std::isfinite(cfg_.attack_ms)) {
        cfg_.attack_ms = 50.0f;
    }
    if (!std::isfinite(cfg_.release_ms)) {
        cfg_.release_ms = 400.0f;
    }
    if (!std::isfinite(cfg_.noise_floor_db)) {
        cfg_.noise_floor_db = -55.0f;
    }
    RecomputeCoeffs();
}

void AutomaticGainControl::RecomputeCoeffs() noexcept {
    target_linear_ = std::pow(10.0f, cfg_.target_db / 20.0f);
    noise_floor_linear_ = std::pow(10.0f, cfg_.noise_floor_db / 20.0f);
    max_gain_linear_ = std::pow(10.0f, cfg_.max_gain_db / 20.0f);
    min_gain_linear_ = std::pow(10.0f, -cfg_.max_gain_db / 20.0f);
    env_coeff_ = TimeToCoeff(kEnvelopeMs, cfg_.sample_rate);
    attack_coeff_ = TimeToCoeff(cfg_.attack_ms, cfg_.sample_rate);
    release_coeff_ = TimeToCoeff(cfg_.release_ms, cfg_.sample_rate);
}

void AutomaticGainControl::Reset() noexcept {
    gain_ = 1.0f; // start at unity (no makeup until the level is known)
    env_ = 0.0f;
}

void AutomaticGainControl::Process(float* interleaved, uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) {
        return;
    }

    const uint32_t ch = cfg_.channels;

    for (uint32_t f = 0; f < frames; ++f) {
        float* frame = interleaved + (static_cast<std::size_t>(f) * ch);

        // Stereo-linked detection: use the loudest channel so the gain decision
        // (and the shared makeup gain) preserves the stereo image.
        float level = 0.0f;
        for (uint32_t c = 0; c < ch; ++c) {
            level = std::max(level, std::fabs(frame[c]));
        }

        // One-pole envelope follower of the program level.
        env_ = level + (env_ - level) * env_coeff_;

        // Noise-floor freeze: below the floor, hold the gain — do NOT boost into
        // silence/noise (which would cause runaway gain). Above the floor, aim
        // the gain at the level that brings the envelope to the target, clamped
        // to the configured boost/attenuation range.
        if (env_ >= noise_floor_linear_) {
            float desired = target_linear_ / std::max(env_, kTinyLevel);
            desired = std::clamp(desired, min_gain_linear_, max_gain_linear_);

            // Attack (fast) when increasing gain toward the desired value,
            // release (slow) when decreasing. One-pole smoothing, same shape as
            // the gate/limiter.
            const float coeff = (desired > gain_) ? attack_coeff_ : release_coeff_;
            gain_ = desired + (gain_ - desired) * coeff;
        }

        // Apply one shared makeup gain to every channel.
        for (uint32_t c = 0; c < ch; ++c) {
            frame[c] *= gain_;
        }
    }
}

} // namespace recorder_core
