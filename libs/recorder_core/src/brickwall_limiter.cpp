#include "brickwall_limiter.h"

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

} // namespace

float LimiterCeilingDbToLinear(float ceiling_db) noexcept {
    if (!std::isfinite(ceiling_db) || ceiling_db >= 0.0f) {
        return 1.0f;
    }
    return std::pow(10.0f, ceiling_db / 20.0f);
}

BrickwallLimiter::BrickwallLimiter(const Config& cfg) {
    Configure(cfg);
}

void BrickwallLimiter::Configure(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.channels == 0) {
        cfg_.channels = 1;
    }
    if (cfg_.sample_rate == 0) {
        cfg_.sample_rate = 48000;
    }
    if (!(cfg_.ceiling_linear > 0.0f) || !std::isfinite(cfg_.ceiling_linear)) {
        cfg_.ceiling_linear = 1.0f;
    }
    RecomputeCoeffs();
}

void BrickwallLimiter::RecomputeCoeffs() noexcept {
    attack_coeff_ = TimeToCoeff(cfg_.attack_ms, cfg_.sample_rate);
    release_coeff_ = TimeToCoeff(cfg_.release_ms, cfg_.sample_rate);
}

void BrickwallLimiter::Reset() noexcept {
    gain_ = 1.0f;
}

void BrickwallLimiter::Process(float* interleaved, uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) {
        return;
    }

    const uint32_t ch = cfg_.channels;
    const float ceiling = cfg_.ceiling_linear;

    for (uint32_t f = 0; f < frames; ++f) {
        float* frame = interleaved + (static_cast<std::size_t>(f) * ch);

        // Stereo-linked detection: use the loudest channel so the stereo image
        // is not shifted by per-channel gain differences.
        float peak = 0.0f;
        for (uint32_t c = 0; c < ch; ++c) {
            peak = std::max(peak, std::fabs(frame[c]));
        }

        // Target gain that would bring this peak exactly to the ceiling.
        const float target = (peak > ceiling) ? (ceiling / peak) : 1.0f;

        // Attack when we need to reduce further (fast), release toward unity
        // when peaks subside (slow). One-pole smoothing.
        const float coeff = (target < gain_) ? attack_coeff_ : release_coeff_;
        gain_ = target + (gain_ - target) * coeff;

        for (uint32_t c = 0; c < ch; ++c) {
            float s = frame[c] * gain_;
            // Brickwall guarantee: the envelope can lag a sudden transient by a
            // sample; clamp catches that residual overshoot.
            if (s > ceiling) {
                s = ceiling;
            } else if (s < -ceiling) {
                s = -ceiling;
            }
            frame[c] = s;
        }
    }
}

} // namespace recorder_core
