#include "mixed_audio_src.h"

#include "discontinuity_gap.h"
#include "output_format_audio_src.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace exosnap::engine {

namespace {
int64_t SamplePosition(uint64_t ns) {
    return static_cast<int64_t>((ns / 1'000'000'000) * MixedAudioSrc::kOutputSampleRate +
                                ((ns % 1'000'000'000) * MixedAudioSrc::kOutputSampleRate + 500'000'000) /
                                    1'000'000'000);
}
uint64_t CaptureClockNs() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return uint64_t(f.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<uint64_t>(counter.QuadPart);
    return (ticks / frequency) * 1'000'000'000 + ((ticks % frequency) * 1'000'000'000) / frequency;
}

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
                             float limiter_ceiling_linear, std::function<uint64_t()> clock_now_ns)
    : sources_(std::move(sources)), source_gain_multipliers_(std::move(source_gain_multipliers)),
      clock_now_ns_(clock_now_ns ? std::move(clock_now_ns) : CaptureClockNs), limiter_enabled_(limiter_enabled),
      limiter_ceiling_linear_((limiter_ceiling_linear > 0.0f) ? limiter_ceiling_linear : 1.0f) {
    for (auto& source : sources_)
        source = std::make_unique<OutputFormatAudioSrc>(std::move(source), kOutputSampleRate, kOutputChannels);
}

void MixedAudioSrc::SetSourceMuted(std::size_t index, bool muted) noexcept {
    if (index >= 32) {
        return;
    }
    const uint32_t bit = 1u << index;
    if (muted) {
        source_mute_mask_.fetch_or(bit, std::memory_order_relaxed);
    } else {
        source_mute_mask_.fetch_and(~bit, std::memory_order_relaxed);
    }
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
    source_degraded_.assign(num, false);
    timeline_sources_.assign(num > 1 ? num : 0, {});
    timeline_started_ = timeline_emitted_ = draining_ = false;
    output_frame_ = latest_frame_ = 0;
    for (auto& source : timeline_sources_) {
        source.samples.resize(static_cast<size_t>(kMaxFifoFrames) * kOutputChannels);
        source.tags.assign(kMaxFifoFrames, std::numeric_limits<int64_t>::min());
    }

    for (size_t i = 0; i < num; ++i) {
        std::string src_err;
        if (!sources_[i]->Init(src_err)) {
            out_error = "MixedAudioSrc::Init: source " + std::to_string(i) + " failed: " + src_err;
            // Shut down every source touched so far, INCLUDING the one that just
            // failed: a failed Init() can still have partially acquired resources
            // (see e.g. WasapiLoopback::Init(), which now self-cleans on every
            // failure path, but that contract isn't guaranteed for every source
            // type) — Shutdown() on an already-clean source is a safe no-op.
            for (size_t k = 0; k <= i; ++k) {
                sources_[k]->Shutdown();
            }
            return false;
        }
    }

    if (num > 1) {
        output_frame_ = latest_frame_ = SamplePosition(clock_now_ns_());
        timeline_started_ = true;
    }
    timeline_discontinuity_pending_ = false;
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
    if (sources_.size() > 1)
        return TimelineFramesReady();
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
    if (draining_)
        return;
    const size_t num = sources_.size();
    const float base_gain = 1.0f / static_cast<float>(num);

    for (size_t i = 0; i < num; ++i) {
        // A degraded inner (endpoint lost) is not polled — it would only fail
        // again. The survivors keep mixing; Reinit reacquires it.
        if (source_degraded_[i]) {
            continue;
        }
        if (sources_[i]->PendingFrameCount() == 0) {
            continue;
        }

        RawAudioBuffer src_buf{};
        std::string src_err;
        if (!sources_[i]->AcquireBuffer(src_buf, src_err)) {
            // Non-empty error == the endpoint was lost (benign "no data this
            // tick" returns false with an empty message). Degrade this inner
            // source visibly instead of silently swallowing it — its
            // contribution becomes honest silence and DegradedSourceCount()
            // surfaces it, until Reinit brings it back.
            if (!src_err.empty()) {
                source_degraded_[i] = true;
            }
            continue;
        }

        if (src_buf.data_discontinuity) {
            any_discontinuity = true;
        }

        // Single-source pass-through of the measured discontinuity gap (H-3): a
        // gain-wrapped single track must not silently drop the inner packet's
        // gap. Hold it for the next emitted buffer. Multi-source merges leave it
        // 0 because timestamped placement already represents each source gap.
        if (num == 1) {
            single_source_pending_gap_frames_ += src_buf.gap_frames;
        }

        if (num > 1) {
            StoreTimelinePacket(i, src_buf);
            sources_[i]->ReleaseBuffer();
            continue;
        }
        const uint32_t frames = src_buf.num_frames;
        auto& fifo = source_fifo_[i];

        if (frames > 0) {
            const size_t base = fifo.size();
            fifo.resize(base + static_cast<size_t>(frames) * kOutputChannels, 0.0f);

            // Silent / null packets occupy the source timeline as literal
            // silence — they are appended as zeros (preserving frame count) so
            // this source's stream stays sample-aligned with the others.
            // A muted inner is treated exactly as a silent packet: the FIFO
            // keeps the zeros it was cleared to, so nothing about the mix
            // cadence or the other sources changes.
            const bool inner_muted = i < 32 && (source_mute_mask_.load(std::memory_order_relaxed) & (1u << i)) != 0;
            if (!inner_muted && !src_buf.silent && src_buf.bytes != nullptr) {
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
    if (draining_)
        return 0;
    uint32_t inner_pending = 0;
    for (size_t i = 0; i < sources_.size(); ++i) {
        // A degraded inner keeps reporting a pending error; skip it so a
        // degraded source is not retried by the packet pump. The shared timeline
        // still becomes ready at its bounded silence deadline.
        if (i < source_degraded_.size() && source_degraded_[i]) {
            continue;
        }
        inner_pending = std::max(inner_pending, sources_[i]->PendingFrameCount());
    }
    return inner_pending;
}

bool MixedAudioSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};
    out_error.clear();

    bool any_discontinuity = false;
    PumpOnePacketPerSource(any_discontinuity);
    timeline_discontinuity_pending_ |= any_discontinuity;

    const uint32_t n = EmittableFrames();
    if (n == 0) {
        // No interval is ready yet. Keep discontinuity pending until emission.
        out_buf.num_frames = 0;
        out_buf.silent = true;
        out_buf.data_discontinuity = any_discontinuity;
        return true;
    }

    const size_t samples = static_cast<size_t>(n) * kOutputChannels;
    mix_buffer_.assign(samples, 0.0f);

    if (sources_.size() > 1) {
        MixTimeline(n);
    } else {
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
    out_buf.data_discontinuity = timeline_discontinuity_pending_;
    timeline_discontinuity_pending_ = false;
    // Single-source: attach the held inner gap (scaled to the 48 kHz output rate)
    // to this emitted buffer, then clear it. The gap precedes these samples, so
    // the audio thread fills it with silence before feeding them.
    if (sources_.size() == 1 && single_source_pending_gap_frames_ > 0) {
        out_buf.gap_frames = ScaleDiscontinuityGapFrames(single_source_pending_gap_frames_, sources_[0]->SampleRate(),
                                                         kOutputSampleRate);
        single_source_pending_gap_frames_ = 0;
    }
    return true;
}

void MixedAudioSrc::StoreTimelinePacket(size_t index, const RawAudioBuffer& packet, bool tail) {
    if (packet.num_frames == 0)
        return;
    auto& source = timeline_sources_[index];
    const auto* normalized = static_cast<OutputFormatAudioSrc*>(sources_[index].get());
    const uint64_t qpc = normalized->LastBufferQpcNs();
    int64_t start;
    if (tail) {
        start = source.end;
    } else if (qpc != 0) {
        start = SamplePosition(qpc);
    } else if (source.seen) {
        start = source.end + packet.gap_frames;
        const auto wall = SamplePosition(clock_now_ns_()) - SamplePosition(kArrivalHorizonNs);
        start = std::max(start, wall);
    } else {
        start = timeline_started_ ? output_frame_ : SamplePosition(clock_now_ns_()) - packet.num_frames;
    }
    if (!timeline_started_) {
        output_frame_ = start;
        latest_frame_ = start;
        timeline_started_ = true;
    } else if (!timeline_emitted_) {
        output_frame_ = std::min(output_frame_, start);
    }
    source.seen = true;
    source.end = start + packet.num_frames;
    latest_frame_ = std::max(latest_frame_, source.end);
    const bool muted = index < 32 && (source_mute_mask_.load(std::memory_order_relaxed) & (1u << index));
    const float gain = source_gain_multipliers_[index] / static_cast<float>(sources_.size());
    const auto* samples = reinterpret_cast<const float*>(packet.bytes);
    for (uint32_t f = 0; f < packet.num_frames; ++f) {
        const int64_t position = start + f;
        if (position < output_frame_ || position >= output_frame_ + kMaxFifoFrames)
            continue;
        const auto slot = static_cast<size_t>(position % kMaxFifoFrames);
        source.tags[slot] = position;
        for (size_t channel = 0; channel < kOutputChannels; ++channel)
            source.samples[slot * kOutputChannels + channel] =
                muted || packet.silent || !samples ? 0.0f : samples[f * kOutputChannels + channel] * gain;
    }
}

uint32_t MixedAudioSrc::TimelineFramesReady() const {
    if (!timeline_started_)
        return 0;
    int64_t ready = latest_frame_;
    if (!draining_) {
        for (size_t i = 0; i < timeline_sources_.size(); ++i) {
            if (source_degraded_[i])
                continue;
            const auto& source = timeline_sources_[i];
            ready = std::min(ready, source.seen ? source.end : output_frame_);
        }
        const int64_t deadline = SamplePosition(clock_now_ns_()) - SamplePosition(kArrivalHorizonNs);
        ready = std::max(ready, deadline);
    }
    return static_cast<uint32_t>(std::clamp<int64_t>(ready - output_frame_, 0, kMixFrameCount));
}

void MixedAudioSrc::MixTimeline(uint32_t frames) {
    last_output_qpc_ns_ = DeviceFramesToNs(static_cast<uint64_t>(output_frame_), kOutputSampleRate);
    for (const auto& source : timeline_sources_) {
        for (uint32_t f = 0; f < frames; ++f) {
            const int64_t position = output_frame_ + f;
            const auto slot = static_cast<size_t>(position % kMaxFifoFrames);
            if (source.tags[slot] != position)
                continue;
            for (size_t channel = 0; channel < kOutputChannels; ++channel)
                mix_buffer_[f * kOutputChannels + channel] += source.samples[slot * kOutputChannels + channel];
        }
    }
    output_frame_ += frames;
    timeline_emitted_ = true;
}

void MixedAudioSrc::BeginDrain() {
    if (draining_)
        return;
    draining_ = true;
    // Already captured filter tails belong immediately after each source's last output.
    for (size_t i = 0; i < sources_.size(); ++i) {
        RawAudioBuffer tail{};
        auto* normalized = static_cast<OutputFormatAudioSrc*>(sources_[i].get());
        if (normalized->DrainResampler(tail) > 0) {
            if (sources_.size() > 1)
                StoreTimelinePacket(i, tail, true);
            else {
                const auto* samples = reinterpret_cast<const float*>(tail.bytes);
                for (size_t s = 0; s < static_cast<size_t>(tail.num_frames) * kOutputChannels; ++s)
                    source_fifo_[0].push_back(samples[s] * source_gain_multipliers_[0]);
            }
        }
    }
}

bool MixedAudioSrc::LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const {
    // Only a single-source mixer can attribute one device clock. The constant
    // FIFO offset (<= one device period) cancels in the estimator's baseline
    // normalization, so forwarding the inner's timing is honest.
    if (sources_.size() != 1) {
        return false;
    }
    return sources_[0]->LastBufferDeviceTiming(out_timing);
}

bool MixedAudioSrc::Reinit(std::string& out_error) {
    out_error.clear();
    std::string first_err;
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (i >= source_degraded_.size() || !source_degraded_[i]) {
            continue;
        }
        std::string err;
        if (sources_[i]->Reinit(err)) {
            source_degraded_[i] = false;
            if (i < timeline_sources_.size())
                timeline_sources_[i].seen = false;
        } else if (first_err.empty()) {
            first_err = err;
        }
    }
    if (!first_err.empty()) {
        out_error = first_err;
    }
    return DegradedSourceCount() == 0;
}

uint32_t MixedAudioSrc::CaptureSourceCount() const {
    return static_cast<uint32_t>(sources_.size());
}

uint32_t MixedAudioSrc::DegradedSourceCount() const {
    uint32_t degraded = 0;
    for (const bool d : source_degraded_) {
        if (d) {
            ++degraded;
        }
    }
    return degraded;
}

uint32_t MixedAudioSrc::DegradedSourceIndexMask() const {
    uint32_t mask = 0;
    for (size_t i = 0; i < source_degraded_.size() && i < 32; ++i) {
        if (source_degraded_[i])
            mask |= 1u << static_cast<uint32_t>(i);
    }
    return mask;
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
    source_degraded_.clear();
    single_source_pending_gap_frames_ = 0;
}

} // namespace exosnap::engine
