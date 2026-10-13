#include "track_audio_mixer.h"
#include "audio_tee.h"
#include "brickwall_limiter.h"
#include "logical_audio_source.h"
#include "output_format_audio_src.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace exosnap::engine {
namespace {
uint64_t ClockNowNs() {
#ifdef _WIN32
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart) * 1000000000ULL +
           static_cast<uint64_t>(counter.QuadPart % frequency.QuadPart) * 1000000000ULL /
               static_cast<uint64_t>(frequency.QuadPart);
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}
int64_t Position(uint64_t qpc, uint32_t rate) {
    return static_cast<int64_t>(qpc / 1000000000ULL) * rate +
           static_cast<int64_t>((qpc % 1000000000ULL) * rate / 1000000000ULL);
}
} // namespace
struct TrackAudioMixer::Impl {
    struct Input {
        std::unique_ptr<OutputFormatAudioSrc> normalized;
        IAudioCaptureSource* reader = nullptr;
        std::vector<float> samples;
        std::vector<int64_t> tags;
        int64_t end = 0;
        bool degraded = false;
        RawAudioBuffer packet;
        uint32_t packet_offset = 0;
        int64_t packet_start = 0;
        bool packet_leased = false, reinitialize = false, flushed = false;
        std::optional<AudioCaptureDescriptor> descriptor;
    };
    ResolvedRoutingTrack track;
    std::vector<std::unique_ptr<IAudioCaptureSource>> pending;
    std::vector<Input> inputs;
    uint32_t rate, channels, block_frames, capacity;
    std::function<uint64_t()> now;
    BrickwallLimiter limiter;
    std::shared_ptr<TrackAudioTelemetry> telemetry = std::make_shared<TrackAudioTelemetry>();
    std::vector<float> output;
    int64_t cursor = 0, latest = 0, finite_end = 0;
    uint64_t output_qpc = 0;
    bool initialized = false, leased = false, emitted = false, draining = false, discontinuity = false;
    std::string name = "Audio track";
    size_t payload_bytes = 0;
    bool drain_failed = false;
    std::optional<uint64_t> undrained_frames;
    uint64_t end_qpc = 0;
    uint64_t start_qpc = 0;
    std::string failure;

    Impl(ResolvedRoutingTrack spec, std::vector<std::unique_ptr<IAudioCaptureSource>> readers, uint32_t sample_rate,
         uint32_t channel_count, std::function<uint64_t()> clock)
        : track(std::move(spec)), pending(std::move(readers)), rate(sample_rate), channels(channel_count),
          block_frames(sample_rate / 100), capacity(sample_rate / 2), now(clock ? std::move(clock) : ClockNowNs) {
        telemetry->snapshot.track_id = track.id;
        telemetry->snapshot.limiter_enabled = track.limiter.enabled;
    }

    void Hold(Input& input, const RawAudioBuffer& packet, uint64_t qpc, bool owns_lease) {
        input.packet = packet;
        input.packet_offset = 0;
        input.packet_start = qpc ? Position(qpc, rate) : (input.end ? input.end : cursor);
        if (!start_qpc && !emitted && qpc && (latest == 0 || input.packet_start < cursor))
            cursor = input.packet_start;
        input.end = input.packet_start + packet.num_frames;
        latest = (std::max)(latest, input.end);
        input.packet_leased = owns_lease;
        discontinuity |= packet.data_discontinuity;
    }
    bool QueuePending(Input& input) {
        const float* pcm = reinterpret_cast<const float*>(input.packet.bytes);
        // Keep the normalized lease until every sample fits ahead of the unread cursor.
        // A large packet or consumer backlog must not wrap onto live ring positions.
        while (input.packet_offset < input.packet.num_frames) {
            const uint32_t f = input.packet_offset;
            const int64_t position = input.packet_start + f;
            if (position >= cursor + capacity)
                return false;
            ++input.packet_offset;
            if (position < cursor)
                continue;
            const size_t slot = static_cast<size_t>(position % capacity);
            input.tags[slot] = position;
            for (uint32_t c = 0; c < channels; ++c)
                input.samples[slot * channels + c] =
                    input.packet.silent || !pcm ? 0 : pcm[static_cast<size_t>(f) * channels + c];
        }
        if (input.packet_leased)
            input.normalized->ReleaseBuffer();
        input.packet = {};
        input.packet_leased = false;
        return true;
    }
    bool Flush(Input& input, std::string& error) {
        RawAudioBuffer tail;
        int64_t undrained = 0;
        if (input.normalized->DrainResampler(tail, &undrained))
            Hold(input, tail, 0, false);
        if (undrained > 0) {
            undrained_frames = undrained_frames.value_or(0) + static_cast<uint64_t>(undrained);
            drain_failed = true;
            error = "Track normalization drain exceeded finite flush bound";
            return false;
        }
        return true;
    }
    bool Collect(std::string& error) {
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto& input = inputs[i];
            if (!input.normalized)
                continue;
            constexpr uint64_t packet_bound = kAudioTeeHorizonNs / kMinimumAudioPacketDurationNs + 16;
            for (uint64_t packets = 0; packets < packet_bound; ++packets) {
                if (!QueuePending(input))
                    break;
                if (input.reinitialize) {
                    if (!input.normalized->Reinit(error)) {
                        input.degraded = true;
                        error = "Track format-generation adaptation failed";
                        return false;
                    }
                    input.reinitialize = false;
                    discontinuity = true;
                }
                if (!input.normalized->PendingFrameCount()) {
                    if (draining && !input.flushed) {
                        input.flushed = true;
                        if (!Flush(input, error))
                            return false;
                        QueuePending(input);
                    }
                    break;
                }
                const auto descriptor = LogicalAudioReaderDescriptor(*input.reader);
                if (descriptor && input.descriptor &&
                    (descriptor->source_generation != input.descriptor->source_generation ||
                     descriptor->format_generation != input.descriptor->format_generation)) {
                    std::vector<AudioCaptureDescriptor> formats;
                    for (const auto& route : inputs)
                        formats.push_back(route.descriptor.value_or(AudioCaptureDescriptor{}));
                    formats[i] = *descriptor;
                    if (TrackAudioMixer::RequiredPayloadBytes(rate, channels, formats) > payload_bytes) {
                        error = "Track generation needs a larger normalization reservation; reprepare required";
                        return false;
                    }
                    input.descriptor = descriptor;
                    input.reinitialize = true;
                    if (!Flush(input, error))
                        return false;
                    continue;
                }
                input.descriptor = descriptor;
                RawAudioBuffer packet;
                if (!input.normalized->AcquireBuffer(packet, error)) {
                    input.degraded = true;
                    return false;
                }
                Hold(input, packet, input.normalized->LastBufferQpcNs(), true);
            }
        }
        return true;
    }
    uint32_t Ready() const {
        if (!initialized || leased)
            return 0;
        int64_t end = latest;
        if (draining)
            end = (std::max)(end, finite_end);
        else {
            const uint64_t qpc = now();
            const int64_t horizon = qpc > 30000000ULL ? Position(qpc - 30000000ULL, rate) : 0;
            end = inputs.size() > 1 ? horizon : (std::max)(end, horizon);
        }
        if (draining)
            end = (std::min)(end, finite_end);
        return end > cursor ? static_cast<uint32_t>((std::min)(end - cursor, static_cast<int64_t>(block_frames))) : 0;
    }
};
TrackAudioMixer::TrackAudioMixer(ResolvedRoutingTrack track, std::vector<std::unique_ptr<IAudioCaptureSource>> inputs,
                                 uint32_t rate, uint32_t channels, std::function<uint64_t()> now, uint64_t start)
    : impl_(std::make_unique<Impl>(std::move(track), std::move(inputs), rate, channels, std::move(now))) {
    impl_->start_qpc = start;
}
TrackAudioMixer::~TrackAudioMixer() {
    Shutdown();
}
std::shared_ptr<TrackAudioTelemetry> TrackAudioMixer::Telemetry() const {
    return impl_->telemetry;
}
size_t TrackAudioMixer::RequiredPayloadBytes(uint32_t rate, uint32_t channels, uint32_t count) {
    if (!count || count > kMaxAudioSources)
        return (std::numeric_limits<size_t>::max)();
    AudioCaptureDescriptor worst;
    worst.sample_rate = 384000;
    worst.channels = 8;
    worst.maximum_packet_frames = 384000;
    worst.sample_format = AudioSampleFormat::Int16;
    std::vector<AudioCaptureDescriptor> descriptors(count, worst);
    return RequiredPayloadBytes(rate, channels, descriptors);
}
size_t TrackAudioMixer::RequiredPayloadBytes(uint32_t rate, uint32_t channels,
                                             std::span<const AudioCaptureDescriptor> inputs) {
    if (rate < 8000 || rate > 384000 || !channels || channels > 8 || inputs.empty() || inputs.size() > kMaxAudioSources)
        return (std::numeric_limits<size_t>::max)();
    size_t bytes = inputs.size() * static_cast<size_t>(rate / 2) * (sizeof(float) * channels + sizeof(int64_t)) +
                   static_cast<size_t>(rate / 100) * channels * sizeof(float);
    for (const auto& descriptor : inputs) {
        if (!descriptor.sample_rate)
            continue;
        if (descriptor.sample_rate < 8000 || descriptor.sample_rate > 384000 || !descriptor.channels ||
            descriptor.channels > 8 || !descriptor.maximum_packet_frames ||
            descriptor.maximum_packet_frames > descriptor.sample_rate)
            return (std::numeric_limits<size_t>::max)();
        if (descriptor.sample_rate == rate && descriptor.channels == channels &&
            descriptor.sample_format == AudioSampleFormat::Float32)
            continue;
        const size_t converted =
            static_cast<size_t>(descriptor.maximum_packet_frames) * rate / descriptor.sample_rate + 8192;
        bytes += 8u * 1024u * 1024u + 3 * converted * channels * sizeof(float) +
                 2 * static_cast<size_t>(descriptor.maximum_packet_frames) * descriptor.channels * sizeof(float);
    }
    return bytes;
}
size_t TrackAudioMixer::ReservedPayloadBytes() const {
    return impl_->payload_bytes;
}
bool TrackAudioMixer::Init(std::string& error) {
    error.clear();
    auto& p = *impl_;
    if (p.initialized)
        return true;
    if (!p.rate || p.rate > 384000 || !p.channels || p.channels > 8 || !p.block_frames || p.pending.empty() ||
        p.pending.size() > kMaxAudioSources || p.pending.size() != p.track.routes.size() ||
        p.track.fixed_average_divisor != p.pending.size()) {
        error = "Track mixer requires an admitted format and one fixed-divisor slot per route";
        return false;
    }
    std::vector<AudioCaptureDescriptor> descriptors;
    for (const auto& source : p.pending) {
        if (!source) {
            descriptors.push_back({});
            continue;
        }
        auto descriptor = LogicalAudioReaderDescriptor(*source);
        if (!descriptor) {
            error = "Track mixer requires processed logical readers";
            return false;
        }
        descriptors.push_back(*descriptor);
    }
    p.payload_bytes = RequiredPayloadBytes(p.rate, p.channels, descriptors);
    if (p.payload_bytes > kAudioRuntimePayloadBytes) {
        error = "Track mixer exceeds bounded timeline payload";
        return false;
    }
    p.inputs.resize(p.pending.size());
    p.cursor = Position(p.start_qpc ? p.start_qpc : p.now(), p.rate);
    for (size_t i = 0; i < p.pending.size(); ++i) {
        if (!std::isfinite(p.track.routes[i].gain_linear) || p.track.routes[i].gain_linear < 0) {
            error = "Track mixer route gain must be finite and nonnegative";
            Shutdown();
            return false;
        }
        auto& input = p.inputs[i];
        input.samples.resize(static_cast<size_t>(p.capacity) * p.channels);
        input.tags.assign(p.capacity, (std::numeric_limits<int64_t>::min)());
        if (!p.pending[i]) {
            input.degraded = true;
            continue;
        }
        if (!p.pending[i]->ClockCorrected()) {
            error = "Track input must already own physical clock correction";
            Shutdown();
            return false;
        }
        input.reader = p.pending[i].get();
        input.descriptor = LogicalAudioReaderDescriptor(*input.reader);
        input.normalized = std::make_unique<OutputFormatAudioSrc>(std::move(p.pending[i]), p.rate, p.channels);
        if (!input.normalized->Init(error)) {
            Shutdown();
            return false;
        }
    }
    p.pending.clear();
    BrickwallLimiter::Config lc;
    lc.sample_rate = p.rate;
    lc.channels = p.channels;
    lc.ceiling_linear = LimiterCeilingDbToLinear(p.track.limiter.ceiling_db);
    p.limiter.Configure(lc);
    p.limiter.Reset();
    {
        std::lock_guard lock(p.telemetry->mutex);
        p.telemetry->snapshot.limiter_gain = p.limiter.CurrentGain();
    }
    p.output.resize(static_cast<size_t>(p.block_frames) * p.channels);
    p.initialized = true;
    return true;
}
bool TrackAudioMixer::Reinit(std::string& error) {
    error.clear();
    // Reader recovery is owned upstream; a track must never reopen shared capture.
    if (!impl_->initialized) {
        error = "Track mixer is not initialized";
        return false;
    }
    return true;
}
uint32_t TrackAudioMixer::PendingFrameCount() {
    if (!impl_->initialized || impl_->leased)
        return 0;
    std::string error;
    if (!impl_->Collect(error)) {
        impl_->failure = error.empty() ? "Track audio normalization failed" : std::move(error);
        return 0;
    }
    return impl_->Ready();
}
std::string TrackAudioMixer::Failure() const {
    return impl_->failure;
}
bool TrackAudioMixer::AcquireBuffer(RawAudioBuffer& buffer, std::string& error) {
    buffer = {};
    error.clear();
    auto& p = *impl_;
    if (!p.initialized || p.leased) {
        error = "Track mixer has no available lease";
        return false;
    }
    if (!p.Collect(error))
        return false;
    const uint32_t frames = p.Ready();
    if (!frames) {
        error = "Track mixer has no pending frames";
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    std::fill(p.output.begin(), p.output.end(), 0.0f);
    for (size_t i = 0; i < p.inputs.size(); ++i) {
        const auto& input = p.inputs[i];
        const float gain = p.track.routes[i].gain_linear / static_cast<float>(p.track.fixed_average_divisor);
        for (uint32_t f = 0; f < frames; ++f) {
            const int64_t position = p.cursor + f;
            const size_t slot = static_cast<size_t>(position % p.capacity);
            if (input.tags[slot] != position)
                continue;
            for (uint32_t c = 0; c < p.channels; ++c)
                p.output[static_cast<size_t>(f) * p.channels + c] += input.samples[slot * p.channels + c] * gain;
        }
    }
    const size_t sample_count = static_cast<size_t>(frames) * p.channels;
    float pre_limiter_peak = 0;
    for (size_t i = 0; i < sample_count; ++i)
        pre_limiter_peak = (std::max)(pre_limiter_peak, std::abs(p.output[i]));
    if (p.track.limiter.enabled)
        p.limiter.Process(p.output.data(), frames);
    double energy = 0;
    float post_limiter_peak = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        const float sample = p.output[i];
        energy += static_cast<double>(sample) * sample;
        post_limiter_peak = (std::max)(post_limiter_peak, std::abs(sample));
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    {
        std::lock_guard lock(p.telemetry->mutex);
        auto& snapshot = p.telemetry->snapshot;
        snapshot.has_measurement = true;
        snapshot.rms = static_cast<float>(std::sqrt(energy / sample_count));
        snapshot.pre_limiter_peak = pre_limiter_peak;
        snapshot.post_limiter_peak = post_limiter_peak;
        snapshot.limiter_gain = p.track.limiter.enabled ? p.limiter.CurrentGain() : 1.f;
        snapshot.processing_time_available = true;
        snapshot.processing_ms += elapsed;
    }
    buffer.bytes = reinterpret_cast<const uint8_t*>(p.output.data());
    buffer.num_frames = frames;
    buffer.silent = post_limiter_peak == 0;
    buffer.data_discontinuity = p.discontinuity;
    p.discontinuity = false;
    p.output_qpc = DeviceFramesToNs(static_cast<uint64_t>(p.cursor), p.rate);
    p.cursor += frames;
    p.emitted = true;
    p.leased = true;
    return true;
}
void TrackAudioMixer::ReleaseBuffer() {
    impl_->leased = false;
}
uint32_t TrackAudioMixer::SampleRate() const {
    return impl_->rate;
}
uint32_t TrackAudioMixer::Channels() const {
    return impl_->channels;
}
AudioSampleFormat TrackAudioMixer::SampleFormat() const {
    return AudioSampleFormat::Float32;
}
const std::string& TrackAudioMixer::EndpointName() const {
    return impl_->name;
}
bool TrackAudioMixer::ClockCorrected() const {
    return true;
}
uint64_t TrackAudioMixer::LastBufferQpcNs() const {
    return impl_->output_qpc;
}
uint32_t TrackAudioMixer::CaptureSourceCount() const {
    return static_cast<uint32_t>(impl_->inputs.size());
}
uint32_t TrackAudioMixer::DegradedSourceCount() const {
    return static_cast<uint32_t>(
        std::count_if(impl_->inputs.begin(), impl_->inputs.end(), [](const auto& x) { return x.degraded; }));
}
void TrackAudioMixer::BeginDrain(uint64_t end) {
    std::string error;
    BeginDrain(end, error);
}
bool TrackAudioMixer::BeginDrain(uint64_t end, std::string& error) {
    auto& p = *impl_;
    error.clear();
    if (p.draining)
        return !p.drain_failed;
    if (!p.initialized) {
        error = "Track mixer is not initialized";
        return false;
    }
    p.draining = true;
    p.finite_end = Position(end, p.rate);
    p.end_qpc = end;
    for (auto& input : p.inputs)
        if (input.reader)
            SealLogicalAudioReaderEnd(*input.reader, end);
    if (!p.Collect(error)) {
        p.drain_failed = true;
        return false;
    }
    return !p.drain_failed;
}
bool TrackAudioMixer::Ended() const {
    return impl_->draining && !impl_->leased && impl_->cursor >= impl_->finite_end;
}
AudioReaderEnd TrackAudioMixer::DrainOutcome() const {
    AudioReaderEnd result;
    result.end_qpc_ns = impl_->end_qpc;
    result.stalled = impl_->drain_failed;
    if (impl_->drain_failed)
        result.undrained_frames = impl_->undrained_frames;
    else if (Ended())
        result.undrained_frames = 0;
    return result;
}
void TrackAudioMixer::Shutdown() {
    if (!impl_)
        return;
    for (auto& input : impl_->inputs)
        if (input.normalized)
            input.normalized->Shutdown();
    impl_->initialized = false;
    impl_->leased = false;
}
} // namespace exosnap::engine
