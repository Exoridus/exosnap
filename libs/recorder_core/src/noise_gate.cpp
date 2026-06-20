#include "noise_gate.h"

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

// Number of frames in `ms` at `sample_rate` (rounded, >= 0).
uint32_t MsToFrames(float ms, uint32_t sample_rate) noexcept {
    if (!(ms > 0.0f) || sample_rate == 0) {
        return 0;
    }
    const float frames = (ms * 0.001f) * static_cast<float>(sample_rate);
    if (!(frames > 0.0f)) {
        return 0;
    }
    return static_cast<uint32_t>(frames + 0.5f);
}

} // namespace

NoiseGate::NoiseGate(const Config& cfg) {
    Configure(cfg);
}

void NoiseGate::Configure(const Config& cfg) {
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
    if (!std::isfinite(cfg_.threshold_db)) {
        cfg_.threshold_db = -45.0f;
    }
    RecomputeCoeffs();
}

void NoiseGate::RecomputeCoeffs() noexcept {
    threshold_linear_ = std::pow(10.0f, cfg_.threshold_db / 20.0f);
    attack_coeff_ = TimeToCoeff(cfg_.attack_ms, cfg_.sample_rate);
    release_coeff_ = TimeToCoeff(cfg_.release_ms, cfg_.sample_rate);
}

void NoiseGate::Reset() noexcept {
    gain_ = 0.0f;
    hold_frames_remaining_ = 0;
}

void NoiseGate::Process(float* interleaved, uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) {
        return;
    }

    const uint32_t ch = cfg_.channels;
    const uint32_t hold_frames = MsToFrames(cfg_.hold_ms, cfg_.sample_rate);

    for (uint32_t f = 0; f < frames; ++f) {
        float* frame = interleaved + (static_cast<std::size_t>(f) * ch);

        // Stereo-linked detection: use the loudest channel so the gate decision
        // (and the shared gain) preserves the stereo image.
        float level = 0.0f;
        for (uint32_t c = 0; c < ch; ++c) {
            level = std::max(level, std::fabs(frame[c]));
        }

        // When the level is above the threshold, (re)arm the hold timer. While
        // the timer is non-zero the gate target stays open even if the level
        // momentarily dips — this keeps the tail of speech and avoids chatter.
        if (level >= threshold_linear_) {
            hold_frames_remaining_ = hold_frames;
        } else if (hold_frames_remaining_ > 0) {
            --hold_frames_remaining_;
        }

        const float target = (hold_frames_remaining_ > 0) ? 1.0f : 0.0f;

        // Attack (fast) when opening toward unity, release (slow) when closing
        // toward silence. One-pole smoothing, same shape as the limiter.
        const float coeff = (target > gain_) ? attack_coeff_ : release_coeff_;
        gain_ = target + (gain_ - target) * coeff;

        // Apply one shared gain to every channel.
        for (uint32_t c = 0; c < ch; ++c) {
            frame[c] *= gain_;
        }
    }
}

} // namespace recorder_core
