#include "mixed_audio_src.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace recorder_core {

namespace {

// Converts src_bytes (up to src_frames) into float32 stereo in dst[0..requested_frames*2-1].
// Remaining dst frames beyond src_frames are left untouched (caller zeroes dst first).
// MVP: for >2 input channels, uses first two channels only.
void ConvertToFloat32Stereo(const uint8_t* src_bytes, uint32_t src_frames, uint32_t src_channels, AudioSampleFormat fmt,
                            float* dst, uint32_t requested_frames) {
    const uint32_t frames = std::min(src_frames, requested_frames);
    constexpr float kInt16Scale = 1.0f / 32768.0f;

    if (fmt == AudioSampleFormat::Float32) {
        const float* src = reinterpret_cast<const float*>(src_bytes);
        if (src_channels == 1) {
            for (uint32_t i = 0; i < frames; ++i) {
                dst[(i * 2) + 0] = src[i];
                dst[(i * 2) + 1] = src[i];
            }
        } else if (src_channels == 2) {
            std::memcpy(dst, src, static_cast<size_t>(frames) * 2u * sizeof(float));
        } else {
            for (uint32_t i = 0; i < frames; ++i) {
                dst[(i * 2) + 0] = src[i * src_channels + 0];
                dst[(i * 2) + 1] = src[i * src_channels + 1];
            }
        }
    } else { // Int16
        const int16_t* src = reinterpret_cast<const int16_t*>(src_bytes);
        if (src_channels == 1) {
            for (uint32_t i = 0; i < frames; ++i) {
                const float v = static_cast<float>(src[i]) * kInt16Scale;
                dst[(i * 2) + 0] = v;
                dst[(i * 2) + 1] = v;
            }
        } else if (src_channels == 2) {
            for (uint32_t i = 0; i < frames; ++i) {
                dst[(i * 2) + 0] = static_cast<float>(src[(i * 2) + 0]) * kInt16Scale;
                dst[(i * 2) + 1] = static_cast<float>(src[(i * 2) + 1]) * kInt16Scale;
            }
        } else {
            for (uint32_t i = 0; i < frames; ++i) {
                dst[(i * 2) + 0] = static_cast<float>(src[i * src_channels + 0]) * kInt16Scale;
                dst[(i * 2) + 1] = static_cast<float>(src[i * src_channels + 1]) * kInt16Scale;
            }
        }
    }
}

} // namespace

MixedAudioSrc::MixedAudioSrc(std::vector<std::unique_ptr<IAudioCaptureSource>> sources,
                             std::vector<float> source_gain_multipliers, bool limiter_enabled,
                             float limiter_ceiling_linear)
    : sources_(std::move(sources)), source_gain_multipliers_(std::move(source_gain_multipliers)),
      limiter_enabled_(limiter_enabled),
      limiter_ceiling_linear_((limiter_ceiling_linear > 0.0f) ? limiter_ceiling_linear : 1.0f) {
}

bool MixedAudioSrc::Init(std::string& out_error) {
    if (sources_.empty()) {
        out_error = "MixedAudioSrc::Init: at least one audio source is required";
        return false;
    }

    if (source_gain_multipliers_.size() != sources_.size()) {
        out_error =
            "MixedAudioSrc::Init: source_gain_multipliers size mismatch (sources=" + std::to_string(sources_.size()) +
            ", gains=" + std::to_string(source_gain_multipliers_.size()) + ")";
        return false;
    }

    const size_t num = sources_.size();
    source_fifo_.assign(num, std::vector<float>{});

    for (size_t i = 0; i < num; ++i) {
        std::string src_err;
        if (!sources_[i]->Init(src_err)) {
            out_error = "MixedAudioSrc::Init: source " + std::to_string(i) + " failed: " + src_err;
            for (size_t k = 0; k < i; ++k) {
                sources_[k]->Shutdown();
            }
            return false;
        }
    }

    mix_buffer_.clear();
    scratch_buffer_.assign(static_cast<size_t>(kMixFrameCount) * kOutputChannels, 0.0f);

    if (limiter_enabled_) {
        BrickwallLimiter::Config lc;
        lc.ceiling_linear = limiter_ceiling_linear_;
        lc.sample_rate = kOutputSampleRate;
        lc.channels = kOutputChannels;
        limiter_.Configure(lc);
        limiter_.Reset();
    }

    initialized_ = true;
    return true;
}

uint32_t MixedAudioSrc::EmittableFrames() const {
    size_t emittable = 0;
    bool any_ready = false;
    for (const auto& fifo : source_fifo_) {
        const size_t frames = fifo.size() / kOutputChannels;
        if (frames == 0) {
            continue;
        }
        if (!any_ready || frames < emittable) {
            emittable = frames;
            any_ready = true;
        }
    }
    return any_ready ? static_cast<uint32_t>(emittable) : 0;
}

void MixedAudioSrc::PumpOnePacketPerSource(bool& any_discontinuity) {
    const size_t num = sources_.size();
    const float base_gain = 1.0f / static_cast<float>(num);

    for (size_t i = 0; i < num; ++i) {
        if (sources_[i]->PendingFrameCount() == 0) {
            continue;
        }

        RawAudioBuffer src_buf{};
        std::string src_err;
        if (!sources_[i]->AcquireBuffer(src_buf, src_err)) {
            continue;
        }

        if (src_buf.data_discontinuity) {
            any_discontinuity = true;
        }

        const uint32_t frames = src_buf.num_frames;
        auto& fifo = source_fifo_[i];

        if (frames > 0) {
            const size_t base = fifo.size();
            fifo.resize(base + static_cast<size_t>(frames) * kOutputChannels, 0.0f);

            // Silent / null packets occupy the source timeline as literal
            // silence — they are appended as zeros (preserving frame count) so
            // this source's stream stays sample-aligned with the others.
            if (!src_buf.silent && src_buf.bytes != nullptr) {
                if (scratch_buffer_.size() < static_cast<size_t>(frames) * kOutputChannels) {
                    scratch_buffer_.assign(static_cast<size_t>(frames) * kOutputChannels, 0.0f);
                }
                std::fill_n(scratch_buffer_.begin(), static_cast<size_t>(frames) * kOutputChannels, 0.0f);
                ConvertToFloat32Stereo(src_buf.bytes, frames, sources_[i]->Channels(), sources_[i]->SampleFormat(),
                                       scratch_buffer_.data(), frames);
                const float gain = base_gain * source_gain_multipliers_[i];
                for (size_t s = 0; s < static_cast<size_t>(frames) * kOutputChannels; ++s) {
                    fifo[base + s] = gain * scratch_buffer_[s];
                }
            }

            // Drift relief: bound the surplus a persistently faster source can
            // accumulate. Drops the oldest frames only past the cap; normal
            // packet jitter never reaches it.
            const size_t cap = static_cast<size_t>(kMaxFifoFrames) * kOutputChannels;
            if (fifo.size() > cap) {
                fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(fifo.size() - cap));
            }
        }

        sources_[i]->ReleaseBuffer();
    }
}

uint32_t MixedAudioSrc::PendingFrameCount() {
    // Side-effect free: report what can be mixed from already-buffered samples,
    // or — when nothing is buffered yet — a positive figure while any source
    // still has an unread packet, so the caller proceeds to AcquireBuffer (which
    // performs the actual pump).
    const uint32_t emittable = EmittableFrames();
    if (emittable > 0) {
        return emittable;
    }
    uint32_t inner_pending = 0;
    for (auto& src : sources_) {
        inner_pending = std::max(inner_pending, src->PendingFrameCount());
    }
    return inner_pending;
}

bool MixedAudioSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};
    out_error.clear();

    bool any_discontinuity = false;
    PumpOnePacketPerSource(any_discontinuity);

    const uint32_t n = EmittableFrames();
    if (n == 0) {
        // No source currently holds samples — emit nothing this call rather than
        // fabricating a silent block. data_discontinuity is still forwarded.
        out_buf.num_frames = 0;
        out_buf.silent = true;
        out_buf.data_discontinuity = any_discontinuity;
        return true;
    }

    const size_t samples = static_cast<size_t>(n) * kOutputChannels;
    mix_buffer_.assign(samples, 0.0f);

    // Sum the first n frames of every source that holds samples; sources with an
    // empty FIFO contribute silence for this block. Then consume n frames from
    // each contributing FIFO, leaving any surplus buffered for the next call.
    for (auto& fifo : source_fifo_) {
        if (fifo.empty()) {
            continue;
        }
        for (size_t s = 0; s < samples; ++s) {
            mix_buffer_[s] += fifo[s];
        }
        fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(samples));
    }

    bool all_zero = true;
    for (float s : mix_buffer_) {
        if (s != 0.0f) {
            all_zero = false;
            break;
        }
    }

    if (limiter_enabled_) {
        // Brickwall limiter: smooth peak reduction to the ceiling (with a final
        // clamp guarantee inside Process) instead of hard-clipping the mix.
        limiter_.Process(mix_buffer_.data(), n);
    } else {
        // Legacy behavior: hard-clip the mix to full scale.
        for (float& s : mix_buffer_) {
            if (s > 1.0f)
                s = 1.0f;
            else if (s < -1.0f)
                s = -1.0f;
        }
    }

    out_buf.bytes = reinterpret_cast<const uint8_t*>(mix_buffer_.data());
    out_buf.num_frames = n;
    out_buf.silent = all_zero;
    out_buf.data_discontinuity = any_discontinuity;
    return true;
}

void MixedAudioSrc::ReleaseBuffer() {
    // Source packets are acquired and released inside PumpOnePacketPerSource;
    // the mixed buffer this exposes is owned by mix_buffer_, so there is nothing
    // further to release here.
}

uint32_t MixedAudioSrc::SampleRate() const {
    return kOutputSampleRate;
}

uint32_t MixedAudioSrc::Channels() const {
    return kOutputChannels;
}

AudioSampleFormat MixedAudioSrc::SampleFormat() const {
    return AudioSampleFormat::Float32;
}

const std::string& MixedAudioSrc::EndpointName() const {
    return endpoint_name_;
}

void MixedAudioSrc::Shutdown() {
    for (auto& src : sources_) {
        src->Shutdown();
    }
    initialized_ = false;
    source_fifo_.clear();
}

} // namespace recorder_core
