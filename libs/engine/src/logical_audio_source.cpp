#include "logical_audio_source.h"
#include "audio_tee.h"
#include "automatic_gain_control.h"
#include "high_pass_filter.h"
#include "noise_gate.h"
#include "rnnoise_denoiser.h"
#include "wasapi_capture_src.h"
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace exosnap::engine {
namespace {
class LogicalReader final : public IAudioCaptureSource {
  public:
    explicit LogicalReader(std::shared_ptr<AudioTeeReader> reader) : reader_(std::move(reader)) {
    }
    bool Init(std::string& error) override {
        error.clear();
        return reader_ != nullptr;
    }
    bool Reinit(std::string& error) override {
        return Init(error);
    }
    uint32_t PendingFrameCount() override {
        return reader_ ? reader_->PendingFrames() : 0;
    }
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string& error) override {
        buffer = {};
        if (!reader_ || leased_) {
            error = "Logical reader has no available lease";
            return false;
        }
        if (!reader_->Acquire(block_, error))
            return false;
        leased_ = true;
        buffer.bytes = reinterpret_cast<const uint8_t*>(block_.samples.data());
        buffer.num_frames = block_.metadata.frames;
        buffer.silent = block_.metadata.silence;
        buffer.data_discontinuity = block_.metadata.discontinuity || block_.reader_overflow_frames != 0;
        const uint64_t maximum = (std::numeric_limits<uint32_t>::max)();
        const uint64_t overflow = (std::min)(block_.reader_overflow_frames, maximum);
        buffer.gap_frames = static_cast<uint32_t>(
            (std::min)(maximum, static_cast<uint64_t>(block_.metadata.device_gap_frames) + overflow));
        return true;
    }
    void ReleaseBuffer() override {
        if (leased_) {
            reader_->Release();
            leased_ = false;
            block_.samples = {};
        }
    }
    uint32_t SampleRate() const override {
        return reader_ ? reader_->Descriptor().sample_rate : 0;
    }
    uint32_t Channels() const override {
        return reader_ ? reader_->Descriptor().channels : 0;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    bool ClockCorrected() const override {
        return reader_ && reader_->Descriptor().clock_corrected;
    }
    uint64_t LastBufferQpcNs() const override {
        return block_.metadata.output_qpc_ns;
    }
    bool LastBufferDeviceTiming(AudioDeviceTiming& timing) const override {
        if (!block_.metadata.device_timing_valid)
            return false;
        timing = block_.metadata.device_timing;
        return true;
    }
    void* BufferReadyEvent() const override {
        return reader_ ? reader_->ReadyEvent() : nullptr;
    }
    void Shutdown() override {
        ReleaseBuffer();
        reader_.reset();
    }
    AudioReaderEnd Seal(uint64_t end) {
        return reader_ ? reader_->SealEnd(end) : AudioReaderEnd{};
    }
    bool Ended() const {
        return !reader_ || reader_->Ended();
    }
    bool Block(AudioBlockView& block) const {
        if (!leased_)
            return false;
        block = block_;
        return true;
    }
    AudioCaptureDescriptor Descriptor() const {
        return reader_ ? reader_->Descriptor() : AudioCaptureDescriptor{};
    }

  private:
    std::shared_ptr<AudioTeeReader> reader_;
    AudioBlockView block_;
    bool leased_ = false;
    std::string name_ = "Logical audio source";
};
bool BuildResampler(SwrContext*& context, uint32_t rate_in, uint32_t rate_out, uint32_t channels, std::string& error) {
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, static_cast<int>(channels));
    const int allocated = swr_alloc_set_opts2(&context, &layout, AV_SAMPLE_FMT_FLT, static_cast<int>(rate_out), &layout,
                                              AV_SAMPLE_FMT_FLT, static_cast<int>(rate_in), 0, nullptr);
    av_channel_layout_uninit(&layout);
    if (allocated < 0 || !context || swr_init(context) < 0) {
        swr_free(&context);
        error = "Logical RNNoise rate adaptation could not initialize";
        return false;
    }
    return true;
}
struct LogicalProcessingTimer {
    LogicalProcessingTimer(std::mutex& meter_mutex, LogicalAudioMeter& source_meter)
        : mutex(meter_mutex), meter(source_meter) {
    }
    std::mutex& mutex;
    LogicalAudioMeter& meter;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    ~LogicalProcessingTimer() {
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        std::lock_guard lock(mutex);
        meter.processing_time_available = true;
        meter.processing_ms += elapsed;
    }
};
} // namespace
struct LogicalAudioSource::Impl {
    ResolvedAudioSource source;
    AudioCaptureDescriptor input_descriptor, descriptor;
    AudioCaptureDescriptor admission;
    size_t reserved_bytes = 0;
    std::shared_ptr<AudioTee> tee;
    HighPassFilter hpf;
    NoiseGate gate;
    AutomaticGainControl agc;
    RnnoiseDenoiser rnnoise;
    mic_channel_detail::AutoModeState auto_mode;
    SwrContext* to_neural = nullptr;
    SwrContext* from_neural = nullptr;
    std::vector<float> mapped, neural, output;
    std::atomic<bool> muted = false;
    mutable std::mutex meter_mutex;
    LogicalAudioMeter meter;
    AudioBlockMetadata last_metadata;
    uint64_t input_frames = 0, output_frames = 0, origin_qpc_ns = 0;
    uint64_t expected_input_end_qpc_ns = 0;
    std::deque<std::pair<uint64_t, uint64_t>> missing_intervals;
    uint32_t priming_frames = RnnoiseDenoiser::kFrameSize;
    bool drained = false, stopped = false, sequence_seen = false;
    uint64_t last_sequence = 0;
    ~Impl() {
        swr_free(&to_neural);
        swr_free(&from_neural);
    }
    bool Configure(std::string& error) {
        HighPassFilter::Config hc;
        hc.cutoff_hz = source.spec.dsp.hpf_cutoff_hz;
        hc.sample_rate = descriptor.sample_rate;
        hc.channels = descriptor.channels;
        hpf.Configure(hc);
        hpf.Reset();
        NoiseGate::Config gc;
        gc.threshold_db = source.spec.dsp.gate_threshold_db;
        gc.sample_rate = descriptor.sample_rate;
        gc.channels = descriptor.channels;
        gate.Configure(gc);
        gate.Reset();
        AutomaticGainControl::Config ac;
        ac.target_db = source.spec.dsp.agc_target_db;
        ac.sample_rate = descriptor.sample_rate;
        ac.channels = descriptor.channels;
        agc.Configure(ac);
        agc.Reset();
        if (source.spec.dsp.rnnoise_enabled) {
            rnnoise.Configure({48000, descriptor.channels});
            if (descriptor.sample_rate != 48000 &&
                (!BuildResampler(to_neural, descriptor.sample_rate, 48000, descriptor.channels, error) ||
                 !BuildResampler(from_neural, 48000, descriptor.sample_rate, descriptor.channels, error)))
                return false;
        }
        return true;
    }
    bool Convert(SwrContext* context, const float* samples, uint32_t frames, std::vector<float>& destination,
                 std::string& error) {
        const int capacity = swr_get_out_samples(context, static_cast<int>(frames));
        const uint64_t bound = static_cast<uint64_t>(admission.maximum_packet_frames) * 6 + 8192;
        if (capacity < 0 || static_cast<uint64_t>(capacity) > bound) {
            error = "Logical RNNoise conversion exceeded admitted scratch bound";
            return false;
        }
        destination.resize(static_cast<size_t>(capacity) * descriptor.channels);
        uint8_t* out = reinterpret_cast<uint8_t*>(destination.data());
        const uint8_t* in = reinterpret_cast<const uint8_t*>(samples);
        const int produced = swr_convert(context, &out, capacity, samples ? &in : nullptr, static_cast<int>(frames));
        if (produced < 0) {
            error = "Logical RNNoise conversion failed";
            return false;
        }
        destination.resize(static_cast<size_t>(produced) * descriptor.channels);
        return true;
    }
    bool Publish(std::vector<float>& pcm, AudioBlockMetadata metadata, std::string& error) {
        uint64_t frames =
            (std::min)(static_cast<uint64_t>(pcm.size() / descriptor.channels), input_frames - output_frames);
        if (!frames)
            return true;
        pcm.resize(static_cast<size_t>(frames) * descriptor.channels);
        double energy = 0;
        float peak = 0;
        size_t sample_index = 0;
        size_t missing_index = 0;
        for (float& sample : pcm) {
            sample *= source.spec.post_dsp_gain_linear;
            if (!std::isfinite(sample)) {
                error = "Logical DSP produced nonfinite PCM";
                return false;
            }
            const uint64_t position = output_frames + sample_index / descriptor.channels;
            while (missing_index < missing_intervals.size() && missing_intervals[missing_index].second <= position)
                ++missing_index;
            if (missing_index < missing_intervals.size() && position >= missing_intervals[missing_index].first)
                sample = 0;
            ++sample_index;
            energy += static_cast<double>(sample) * sample;
            peak = (std::max)(peak, std::abs(sample));
        }
        {
            std::lock_guard lock(meter_mutex);
            meter.has_measurement = true;
            meter.rms = static_cast<float>(std::sqrt(energy / pcm.size()));
            meter.peak = peak;
            meter.source_generation = descriptor.source_generation;
        }
        const bool silence =
            muted.load(std::memory_order_relaxed) || !source.spec.participation || source.silence_for_this_start;
        if (silence)
            std::fill(pcm.begin(), pcm.end(), 0.f);
        metadata.silence = silence || peak == 0;
        if (source.spec.dsp.rnnoise_enabled) {
            metadata.filter_latency_ns += 10000000ULL;
            if (to_neural)
                metadata.filter_latency_ns += static_cast<uint64_t>(swr_get_delay(to_neural, 1000000000));
            if (from_neural)
                metadata.filter_latency_ns += static_cast<uint64_t>(swr_get_delay(from_neural, 1000000000));
            metadata.output_qpc_ns =
                origin_qpc_ns ? origin_qpc_ns + DeviceFramesToNs(output_frames, descriptor.sample_rate) : 0;
            metadata.device_timing_valid = false;
            metadata.native_sample_position_valid = false;
        }
        size_t offset = 0;
        while (frames) {
            const uint32_t count =
                static_cast<uint32_t>((std::min)(frames, static_cast<uint64_t>(descriptor.maximum_packet_frames)));
            metadata.frames = count;
            if (!tee->Publish(
                    std::span<const float>(pcm.data() + offset, static_cast<size_t>(count) * descriptor.channels),
                    metadata, error))
                return false;
            output_frames += count;
            frames -= count;
            offset += static_cast<size_t>(count) * descriptor.channels;
            if (metadata.output_qpc_ns)
                metadata.output_qpc_ns += DeviceFramesToNs(count, descriptor.sample_rate);
            metadata.device_gap_frames = 0;
            metadata.discontinuity = false;
        }
        while (!missing_intervals.empty() && missing_intervals.front().second <= output_frames)
            missing_intervals.pop_front();
        return true;
    }
    bool NeuralOutput(std::vector<float>& pcm, const AudioBlockMetadata& metadata, std::string& error) {
        const uint32_t frames = static_cast<uint32_t>(pcm.size() / descriptor.channels);
        if (!frames)
            return true;
        rnnoise.Process(pcm.data(), frames);
        // Priming silence is processing latency, not captured audio. Drain recovers the delayed real tail.
        const uint32_t discard = (std::min)(priming_frames, frames);
        priming_frames -= discard;
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(discard) * descriptor.channels);
        if (pcm.empty())
            return true;
        if (from_neural) {
            if (!Convert(from_neural, pcm.data(), static_cast<uint32_t>(pcm.size() / descriptor.channels), output,
                         error))
                return false;
            return Publish(output, metadata, error);
        }
        return Publish(pcm, metadata, error);
    }
    bool ProcessPcm(std::span<const float> samples, const AudioBlockMetadata& metadata, bool synthetic_gap,
                    std::string& error) {
        LogicalProcessingTimer timer{meter_mutex, meter};
        input_frames += metadata.frames;
        mapped.assign(static_cast<size_t>(metadata.frames) * descriptor.channels, 0.f);
        if (!metadata.silence) {
            for (uint32_t f = 0; f < metadata.frames; ++f)
                for (uint32_t c = 0; c < descriptor.channels; ++c)
                    mapped[static_cast<size_t>(f) * descriptor.channels + c] =
                        samples[static_cast<size_t>(f) * input_descriptor.channels +
                                (std::min)(c, input_descriptor.channels - 1)];
        }
        if (source.spec.binding.kind == AudioBindingKind::MicrophoneEndpoint && descriptor.channels == 2) {
            auto mode = source.spec.channel_mode;
            if (mode == MicChannelMode::Auto) {
                if (!synthetic_gap)
                    mic_channel_detail::UpdateAutoModeStateFloat32(auto_mode, mapped.data(), metadata.frames,
                                                                   metadata.silence);
                mode = mic_channel_detail::EffectiveAutoMode(auto_mode);
            }
            for (uint32_t f = 0; f < metadata.frames; ++f) {
                float& l = mapped[static_cast<size_t>(f) * 2];
                float& r = mapped[static_cast<size_t>(f) * 2 + 1];
                mic_channel_detail::MapStereoFrameFloat32(mode, l, r, l, r);
            }
        }
        if (source.spec.dsp.hpf_enabled)
            hpf.Process(mapped.data(), metadata.frames);
        if (source.spec.dsp.gate_enabled)
            gate.Process(mapped.data(), metadata.frames);
        if (source.spec.dsp.agc_enabled)
            agc.Process(mapped.data(), metadata.frames);
        {
            std::lock_guard lock(meter_mutex);
            ++meter.processed_blocks;
            meter.processed_frames += metadata.frames;
        }
        if (!source.spec.dsp.rnnoise_enabled)
            return Publish(mapped, metadata, error);
        if (to_neural) {
            if (!Convert(to_neural, mapped.data(), metadata.frames, neural, error))
                return false;
            return NeuralOutput(neural, metadata, error);
        }
        return NeuralOutput(mapped, metadata, error);
    }
};
LogicalAudioSource::LogicalAudioSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
LogicalAudioSource::~LogicalAudioSource() {
    Stop();
}
size_t LogicalAudioSource::RequiredPayloadBytes(const AudioCaptureDescriptor& descriptor) {
    ResolvedAudioSource source;
    source.spec.dsp.rnnoise_enabled = true;
    return RequiredPayloadBytes(source, descriptor);
}
AudioCaptureDescriptor LogicalAudioSource::ProjectOutputDescriptor(const ResolvedAudioSource& source,
                                                                   AudioCaptureDescriptor input) {
    if (source.spec.binding.kind == AudioBindingKind::MicrophoneEndpoint && input.channels == 1)
        input.channels = 2;
    input.sample_format = AudioSampleFormat::Float32;
    return input;
}
size_t LogicalAudioSource::RequiredPayloadBytes(const ResolvedAudioSource& source, AudioCaptureDescriptor descriptor) {
    if (descriptor.sample_rate < 8000 || descriptor.sample_rate > 384000 || !descriptor.channels ||
        descriptor.channels > 8 || !descriptor.maximum_packet_frames ||
        descriptor.maximum_packet_frames > descriptor.sample_rate)
        return (std::numeric_limits<size_t>::max)();
    descriptor.channels = (std::max)(descriptor.channels, 2u);
    const size_t tee = AudioTee::RequiredPayloadBytes(descriptor);
    const size_t frames = descriptor.maximum_packet_frames;
    // Reserve both conversion scratch and library FIFO/filter state across the supported rate envelope.
    const size_t dsp = source.spec.dsp.rnnoise_enabled
                           ? (20 * frames + 16384) * descriptor.channels * sizeof(float) + 8u * 1024u * 1024u
                           : frames * descriptor.channels * sizeof(float) + 64u * 1024u;
    if (tee > (std::numeric_limits<size_t>::max)() - dsp)
        return (std::numeric_limits<size_t>::max)();
    return tee + dsp;
}
std::unique_ptr<LogicalAudioSource>
LogicalAudioSource::Create(ResolvedAudioSource source, const AudioCaptureDescriptor& descriptor, std::string& error) {
    error.clear();
    const auto& dsp = source.spec.dsp;
    if (source.spec.id == AudioSourceId{} ||
        RequiredPayloadBytes(source, descriptor) == (std::numeric_limits<size_t>::max)() || !descriptor.channels ||
        descriptor.channels > 8 || !descriptor.maximum_packet_frames ||
        descriptor.sample_format != AudioSampleFormat::Float32 || !std::isfinite(source.spec.post_dsp_gain_linear) ||
        source.spec.post_dsp_gain_linear < 0 || !std::isfinite(dsp.hpf_cutoff_hz) ||
        !std::isfinite(dsp.gate_threshold_db) || !std::isfinite(dsp.agc_target_db)) {
        error = "Logical source requires a valid identity and finite admitted Float32 format/configuration";
        return {};
    }
    auto impl = std::make_unique<Impl>();
    impl->source = std::move(source);
    impl->input_descriptor = descriptor;
    impl->descriptor = ProjectOutputDescriptor(impl->source, descriptor);
    impl->admission = descriptor;
    impl->reserved_bytes = RequiredPayloadBytes(impl->source, descriptor);
    impl->muted = impl->source.spec.muted;
    impl->meter.source_id = impl->source.spec.id;
    impl->meter.source_generation = descriptor.source_generation;
    impl->meter.clock_id = descriptor.clock_id;
    impl->tee = AudioTee::Create(impl->descriptor, error);
    if (!impl->tee || !impl->Configure(error))
        return {};
    return std::unique_ptr<LogicalAudioSource>(new LogicalAudioSource(std::move(impl)));
}
bool LogicalAudioSource::Process(const AudioBlockView& input, std::string& error) {
    error.clear();
    auto& p = *impl_;
    const auto& d = input.descriptor;
    if (d.source_generation != p.input_descriptor.source_generation ||
        d.format_generation != p.input_descriptor.format_generation)
        if (!Reconfigure(d, error))
            return false;
    if (p.stopped || p.drained || d.sample_rate != p.input_descriptor.sample_rate ||
        d.channels != p.input_descriptor.channels || d.source_generation != p.input_descriptor.source_generation ||
        d.format_generation != p.input_descriptor.format_generation ||
        d.clock_corrected != p.input_descriptor.clock_corrected || d.sample_format != AudioSampleFormat::Float32 ||
        input.metadata.frames > p.input_descriptor.maximum_packet_frames ||
        (!input.metadata.silence && input.samples.size() != static_cast<size_t>(input.metadata.frames) * d.channels) ||
        (!input.samples.empty() && input.samples.size() != static_cast<size_t>(input.metadata.frames) * d.channels) ||
        (p.sequence_seen && input.metadata.sequence <= p.last_sequence)) {
        error = "Logical source block violates frozen format, generation, sequence or packet admission";
        return false;
    }
    if (!input.metadata.frames)
        return true;
    if (!input.metadata.silence &&
        !std::all_of(input.samples.begin(), input.samples.end(), [](float sample) { return std::isfinite(sample); })) {
        error = "Logical source input PCM must be finite";
        return false;
    }
    const uint64_t limit = (std::numeric_limits<uint64_t>::max)();
    if (input.reader_overflow_frames > limit - input.metadata.device_gap_frames) {
        error = "Logical missing-frame count exhausted";
        return false;
    }
    uint64_t gap = input.reader_overflow_frames + input.metadata.device_gap_frames;
    if (p.expected_input_end_qpc_ns && input.metadata.output_qpc_ns > p.expected_input_end_qpc_ns) {
        const uint64_t elapsed = input.metadata.output_qpc_ns - p.expected_input_end_qpc_ns;
        // Ignore sub-millisecond packet timestamp noise; physical compensation remains upstream.
        if (elapsed >= 1000000ULL) {
            const uint64_t inferred = (elapsed / 1000000000ULL) * d.sample_rate +
                                      ((elapsed % 1000000000ULL) * d.sample_rate + 500000000ULL) / 1000000000ULL;
            gap = (std::max)(gap, inferred);
        }
    }
    {
        std::lock_guard lock(p.meter_mutex);
        if (gap > limit - p.meter.missing_frames) {
            error = "Logical missing-frame count exhausted";
            return false;
        }
        p.meter.missing_frames += gap;
        p.meter.reader_overflow_frames += input.reader_overflow_frames;
    }
    auto metadata = input.metadata;
    if (gap && p.input_frames) {
        metadata.discontinuity = true;
        const uint64_t warmup_limit =
            (std::min)(2ULL * d.sample_rate, static_cast<uint64_t>(p.input_descriptor.maximum_packet_frames) * 256);
        if (gap > warmup_limit || p.missing_intervals.size() >= 4096) {
            if (!Drain(error))
                return false;
            swr_free(&p.to_neural);
            swr_free(&p.from_neural);
            if (!p.Configure(error))
                return false;
            // History across a long unavailable interval is reset rather than replayed without a bound.
            p.input_frames = p.output_frames = 0;
            p.origin_qpc_ns = metadata.output_qpc_ns;
            p.priming_frames = RnnoiseDenoiser::kFrameSize;
            p.drained = false;
            p.missing_intervals.clear();
            std::lock_guard lock(p.meter_mutex);
            ++p.meter.history_reset_count;
        } else {
            if (gap > limit - p.input_frames) {
                error = "Logical sample timeline exhausted";
                return false;
            }
            p.missing_intervals.emplace_back(p.input_frames, p.input_frames + gap);
            uint64_t remaining = gap, offset = 0;
            while (remaining) {
                AudioBlockMetadata silence = metadata;
                silence.frames = static_cast<uint32_t>(
                    (std::min)(remaining, static_cast<uint64_t>(p.input_descriptor.maximum_packet_frames)));
                silence.output_qpc_ns = p.expected_input_end_qpc_ns
                                            ? p.expected_input_end_qpc_ns + DeviceFramesToNs(offset, d.sample_rate)
                                            : 0;
                silence.capture_qpc_ns = 0;
                silence.silence = true;
                silence.device_gap_frames = 0;
                silence.device_timing_valid = false;
                silence.native_sample_position_valid = false;
                if (!p.ProcessPcm({}, silence, true, error))
                    return false;
                remaining -= silence.frames;
                offset += silence.frames;
            }
        }
    }
    // Missing intervals are already explicit silent PCM or QPC holes; compatibility consumers must not fill them again.
    metadata.device_gap_frames = 0;
    if (!p.input_frames)
        p.origin_qpc_ns = metadata.output_qpc_ns;
    p.sequence_seen = true;
    p.last_sequence = input.metadata.sequence;
    p.last_metadata = metadata;
    if (metadata.frames > limit - p.input_frames) {
        error = "Logical sample timeline exhausted";
        return false;
    }
    if (!p.ProcessPcm(input.samples, metadata, false, error))
        return false;
    if (metadata.output_qpc_ns) {
        const uint64_t duration = DeviceFramesToNs(metadata.frames, d.sample_rate);
        if (metadata.output_qpc_ns > limit - duration) {
            error = "Logical QPC timeline exhausted";
            return false;
        }
        p.expected_input_end_qpc_ns = metadata.output_qpc_ns + duration;
    }
    return true;
}
std::vector<std::shared_ptr<AudioTeeReader>> LogicalAudioSource::AttachReaders(uint32_t count, uint64_t qpc,
                                                                               std::string& error) {
    return impl_->tee->AttachReaders(count, qpc, error);
}
void LogicalAudioSource::SetMuted(bool muted) noexcept {
    impl_->muted.store(muted, std::memory_order_relaxed);
}
LogicalAudioMeter LogicalAudioSource::Meter() const {
    const auto reader_count = impl_->tee->ReaderCount();
    std::lock_guard lock(impl_->meter_mutex);
    auto result = impl_->meter;
    result.muted = impl_->muted.load(std::memory_order_relaxed);
    result.reader_count = reader_count;
    return result;
}
AudioCaptureDescriptor LogicalAudioSource::Descriptor() const {
    return impl_->descriptor;
}
size_t LogicalAudioSource::ReservedPayloadBytes() const {
    return impl_->reserved_bytes;
}
bool LogicalAudioSource::Reconfigure(const AudioCaptureDescriptor& descriptor, std::string& error) {
    error.clear();
    auto& p = *impl_;
    const auto& old = p.input_descriptor;
    if (p.stopped || descriptor.source_generation < old.source_generation ||
        descriptor.format_generation < old.format_generation ||
        (descriptor.source_generation == old.source_generation &&
         descriptor.format_generation == old.format_generation) ||
        descriptor.sample_rate < 8000 || descriptor.sample_rate > p.admission.sample_rate || !descriptor.channels ||
        descriptor.channels > p.admission.channels || !descriptor.maximum_packet_frames ||
        descriptor.maximum_packet_frames > p.admission.maximum_packet_frames ||
        descriptor.maximum_packet_frames > descriptor.sample_rate ||
        descriptor.sample_format != AudioSampleFormat::Float32 ||
        descriptor.clock_corrected != p.admission.clock_corrected) {
        error = "Logical format generation exceeds admission or does not advance; reprepare required";
        return false;
    }
    if (!Drain(error))
        return false;
    swr_free(&p.to_neural);
    swr_free(&p.from_neural);
    p.input_descriptor = descriptor;
    p.descriptor = ProjectOutputDescriptor(p.source, descriptor);
    if (!p.tee->SetDescriptor(p.descriptor, error) || !p.Configure(error)) {
        p.stopped = true;
        p.tee->Stop();
        return false;
    }
    p.auto_mode = {};
    p.input_frames = p.output_frames = p.origin_qpc_ns = 0;
    p.expected_input_end_qpc_ns = 0;
    p.missing_intervals.clear();
    p.priming_frames = RnnoiseDenoiser::kFrameSize;
    p.drained = false;
    p.sequence_seen = false;
    {
        std::lock_guard lock(p.meter_mutex);
        p.meter.has_measurement = false;
        p.meter.source_generation = descriptor.source_generation;
        p.meter.clock_id = descriptor.clock_id;
    }
    return true;
}
bool LogicalAudioSource::Drain(std::string& error) {
    error.clear();
    auto& p = *impl_;
    if (p.drained)
        return true;
    if (p.stopped) {
        error = "Logical source is stopped";
        return false;
    }
    if (p.source.spec.dsp.rnnoise_enabled && p.input_frames) {
        LogicalProcessingTimer timer{p.meter_mutex, p.meter};
        if (p.to_neural) {
            for (int i = 0; i < 16; ++i) {
                if (!p.Convert(p.to_neural, nullptr, 0, p.neural, error))
                    return false;
                if (p.neural.empty())
                    break;
                if (!p.NeuralOutput(p.neural, p.last_metadata, error))
                    return false;
            }
        }
        p.neural.assign(static_cast<size_t>(RnnoiseDenoiser::kFrameSize) * p.descriptor.channels, 0);
        if (!p.NeuralOutput(p.neural, p.last_metadata, error))
            return false;
        if (p.from_neural) {
            for (int i = 0; i < 16; ++i) {
                if (!p.Convert(p.from_neural, nullptr, 0, p.output, error))
                    return false;
                if (p.output.empty())
                    break;
                if (!p.Publish(p.output, p.last_metadata, error))
                    return false;
            }
        }
        if (p.output_frames != p.input_frames) {
            error = "Logical RNNoise drain did not reach finite input boundary";
            return false;
        }
    }
    p.drained = true;
    return true;
}
void LogicalAudioSource::Stop() {
    if (impl_ && !impl_->stopped) {
        impl_->stopped = true;
        impl_->tee->Stop();
    }
}
std::unique_ptr<IAudioCaptureSource> MakeLogicalAudioReader(std::shared_ptr<AudioTeeReader> reader) {
    return reader ? std::make_unique<LogicalReader>(std::move(reader)) : nullptr;
}
AudioReaderEnd SealLogicalAudioReaderEnd(IAudioCaptureSource& reader, uint64_t end) {
    auto* logical = dynamic_cast<LogicalReader*>(&reader);
    return logical ? logical->Seal(end) : AudioReaderEnd{};
}
bool LogicalAudioReaderEnded(const IAudioCaptureSource& reader) {
    const auto* logical = dynamic_cast<const LogicalReader*>(&reader);
    return logical && logical->Ended();
}
bool LastLogicalAudioBlock(const IAudioCaptureSource& reader, AudioBlockView& block) {
    const auto* logical = dynamic_cast<const LogicalReader*>(&reader);
    return logical && logical->Block(block);
}
std::optional<AudioCaptureDescriptor> LogicalAudioReaderDescriptor(const IAudioCaptureSource& reader) {
    const auto* logical = dynamic_cast<const LogicalReader*>(&reader);
    return logical ? std::optional<AudioCaptureDescriptor>(logical->Descriptor()) : std::nullopt;
}
} // namespace exosnap::engine
