#include "high_pass_filter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace recorder_core {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Butterworth Q for a 2nd-order section (maximally flat passband).
constexpr float kButterworthQ = 0.70710678118654752440f; // 1 / sqrt(2)

} // namespace

HighPassFilter::HighPassFilter(const Config& cfg) {
    Configure(cfg);
}

void HighPassFilter::Configure(const Config& cfg) {
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

    const float nyquist = static_cast<float>(cfg_.sample_rate) * 0.5f;
    if (!std::isfinite(cfg_.cutoff_hz) || !(cfg_.cutoff_hz > 0.0f)) {
        cfg_.cutoff_hz = 80.0f;
    }
    // Keep the cutoff strictly inside (0, Nyquist) so the bilinear transform is
    // well-defined. A 0.999 fraction avoids tan() blowing up at exactly Nyquist.
    const float max_cutoff = nyquist * 0.999f;
    if (cfg_.cutoff_hz > max_cutoff) {
        cfg_.cutoff_hz = max_cutoff;
    }

    RecomputeCoeffs();
}

void HighPassFilter::RecomputeCoeffs() noexcept {
    // RBJ audio-EQ cookbook — high-pass.
    const float w0 = 2.0f * kPi * cfg_.cutoff_hz / static_cast<float>(cfg_.sample_rate);
    const float cos_w0 = std::cos(w0);
    const float sin_w0 = std::sin(w0);
    const float alpha = sin_w0 / (2.0f * kButterworthQ);

    const float b0 = (1.0f + cos_w0) * 0.5f;
    const float b1 = -(1.0f + cos_w0);
    const float b2 = (1.0f + cos_w0) * 0.5f;
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cos_w0;
    const float a2 = 1.0f - alpha;

    const float inv_a0 = (a0 != 0.0f) ? (1.0f / a0) : 1.0f;
    b0_ = b0 * inv_a0;
    b1_ = b1 * inv_a0;
    b2_ = b2 * inv_a0;
    a1_ = a1 * inv_a0;
    a2_ = a2 * inv_a0;
}

void HighPassFilter::Reset() noexcept {
    z1_.fill(0.0f);
    z2_.fill(0.0f);
}

void HighPassFilter::Process(float* interleaved, uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0) {
        return;
    }

    const uint32_t ch = cfg_.channels;
    for (uint32_t f = 0; f < frames; ++f) {
        float* frame = interleaved + (static_cast<std::size_t>(f) * ch);
        for (uint32_t c = 0; c < ch; ++c) {
            const float x = frame[c];
            // Transposed Direct Form II.
            const float y = b0_ * x + z1_[c];
            z1_[c] = b1_ * x - a1_ * y + z2_[c];
            z2_[c] = b2_ * x - a2_ * y;
            frame[c] = y;
        }
    }
}

} // namespace recorder_core
