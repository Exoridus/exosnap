#include "rnnoise_denoiser.h"

#include <rnnoise.h>

#include <algorithm>
#include <cstddef>

namespace recorder_core {

namespace {

// RNNoise operates on samples scaled to the int16 range, so a normalized [-1, 1]
// float must be multiplied by 32768 going in and divided by 32768 coming out.
constexpr float kInt16Scale = 32768.0f;
constexpr float kInt16InvScale = 1.0f / 32768.0f;

} // namespace

RnnoiseDenoiser::RnnoiseDenoiser(const Config& cfg) {
    Configure(cfg);
}

RnnoiseDenoiser::~RnnoiseDenoiser() {
    DestroyStates();
}

void RnnoiseDenoiser::DestroyStates() noexcept {
    for (DenoiseState* st : states_) {
        if (st != nullptr) {
            rnnoise_destroy(st);
        }
    }
    states_.clear();
}

void RnnoiseDenoiser::RecreateStates() {
    DestroyStates();
    states_.resize(cfg_.channels, nullptr);
    if (active_) {
        for (uint32_t c = 0; c < cfg_.channels; ++c) {
            // NULL model => RNNoise's built-in default model.
            states_[c] = rnnoise_create(nullptr);
        }
    }
}

void RnnoiseDenoiser::Configure(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.channels == 0) {
        cfg_.channels = 1;
    }
    if (cfg_.channels > kMaxChannels) {
        cfg_.channels = kMaxChannels;
    }
    if (cfg_.sample_rate == 0) {
        cfg_.sample_rate = kSampleRate;
    }

    // RNNoise only supports 48 kHz. At any other rate the stage is a no-op
    // passthrough (no states, no buffering).
    active_ = (cfg_.sample_rate == kSampleRate);

    RecreateStates();
    Reset();
}

void RnnoiseDenoiser::Reset() noexcept {
    in_accum_.assign(cfg_.channels, {});
    out_fifo_.assign(cfg_.channels, {});
    // One block of priming silence per channel: the inherent block latency.
    priming_remaining_.assign(cfg_.channels, active_ ? kFrameSize : 0u);
}

void RnnoiseDenoiser::Process(float* interleaved, uint32_t frames) noexcept {
    if (interleaved == nullptr || frames == 0 || !active_) {
        return;
    }

    const uint32_t ch = cfg_.channels;

    for (uint32_t c = 0; c < ch; ++c) {
        DenoiseState* st = states_[c];
        if (st == nullptr) {
            continue; // defensive: a creation failure leaves this channel as passthrough
        }

        std::vector<float>& accum = in_accum_[c];
        std::vector<float>& fifo = out_fifo_[c];

        // 1) De-interleave this channel's input into its accumulator.
        accum.reserve(accum.size() + frames);
        for (uint32_t f = 0; f < frames; ++f) {
            accum.push_back(interleaved[static_cast<std::size_t>(f) * ch + c]);
        }

        // 2) Process every whole 480-sample block now available: scale up to the
        //    int16 range, denoise, scale back down, push to the output FIFO.
        std::size_t consumed = 0;
        while (accum.size() - consumed >= kFrameSize) {
            float block[kFrameSize];
            float denoised[kFrameSize];
            for (uint32_t i = 0; i < kFrameSize; ++i) {
                block[i] = accum[consumed + i] * kInt16Scale;
            }
            rnnoise_process_frame(st, denoised, block);
            const std::size_t base = fifo.size();
            fifo.resize(base + kFrameSize);
            for (uint32_t i = 0; i < kFrameSize; ++i) {
                fifo[base + i] = denoised[i] * kInt16InvScale;
            }
            consumed += kFrameSize;
        }
        if (consumed > 0) {
            accum.erase(accum.begin(), accum.begin() + static_cast<std::ptrdiff_t>(consumed));
        }

        // 3) Emit exactly `frames` denoised samples in place: priming silence
        //    first (the one-block latency), then from the FIFO front. The
        //    invariant priming + produced >= input_total guarantees the FIFO has
        //    enough samples once priming is exhausted.
        std::size_t fifo_read = 0;
        for (uint32_t f = 0; f < frames; ++f) {
            float out_sample = 0.0f;
            if (priming_remaining_[c] > 0) {
                priming_remaining_[c]--;
            } else if (fifo_read < fifo.size()) {
                out_sample = fifo[fifo_read++];
            }
            interleaved[static_cast<std::size_t>(f) * ch + c] = out_sample;
        }
        if (fifo_read > 0) {
            fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(fifo_read));
        }
    }
}

} // namespace recorder_core
