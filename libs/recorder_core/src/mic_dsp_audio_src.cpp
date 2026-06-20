#include "mic_dsp_audio_src.h"

#include <algorithm>
#include <cstddef>

namespace recorder_core {

namespace {

constexpr float kInt16Scale = 1.0f / 32768.0f;

// Convert `frames` interleaved samples of the inner format to interleaved
// Float32 in `dst`, preserving the channel count.
void ConvertToFloat32(const uint8_t* src_bytes, uint32_t frames, uint32_t channels, AudioSampleFormat fmt, float* dst) {
    const std::size_t count = static_cast<std::size_t>(frames) * channels;
    if (fmt == AudioSampleFormat::Float32) {
        const float* src = reinterpret_cast<const float*>(src_bytes);
        std::copy(src, src + count, dst);
    } else { // Int16
        const int16_t* src = reinterpret_cast<const int16_t*>(src_bytes);
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = static_cast<float>(src[i]) * kInt16Scale;
        }
    }
}

} // namespace

MicDspAudioSrc::MicDspAudioSrc(std::unique_ptr<IAudioCaptureSource> inner, const MicDspConfig& cfg)
    : inner_(std::move(inner)), cfg_(cfg) {
}

bool MicDspAudioSrc::Init(std::string& out_error) {
    if (inner_ == nullptr) {
        out_error = "MicDspAudioSrc::Init: inner source is null";
        return false;
    }
    if (!inner_->Init(out_error)) {
        return false;
    }

    channels_ = inner_->Channels();
    sample_rate_ = inner_->SampleRate();
    if (channels_ == 0) {
        channels_ = 1;
    }
    if (sample_rate_ == 0) {
        sample_rate_ = 48000;
    }

    HighPassFilter::Config hc;
    hc.cutoff_hz = cfg_.hpf_cutoff_hz;
    hc.sample_rate = sample_rate_;
    hc.channels = channels_;
    hpf_.Configure(hc);
    hpf_.Reset();

    NoiseGate::Config gc;
    gc.threshold_db = cfg_.gate_threshold_db;
    gc.sample_rate = sample_rate_;
    gc.channels = channels_;
    gate_.Configure(gc);
    gate_.Reset();

    initialized_ = true;
    return true;
}

uint32_t MicDspAudioSrc::PendingFrameCount() {
    return inner_->PendingFrameCount();
}

bool MicDspAudioSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};
    out_error.clear();

    RawAudioBuffer src_buf{};
    if (!inner_->AcquireBuffer(src_buf, out_error)) {
        return false;
    }

    // Pass-through for silent / empty buffers: nothing to process, preserve flags.
    if (src_buf.silent || src_buf.bytes == nullptr || src_buf.num_frames == 0) {
        out_buf = src_buf;
        return true;
    }

    const std::size_t needed = static_cast<std::size_t>(src_buf.num_frames) * channels_;
    if (scratch_buffer_.size() < needed) {
        scratch_buffer_.assign(needed, 0.0f);
    }

    ConvertToFloat32(src_buf.bytes, src_buf.num_frames, channels_, inner_->SampleFormat(), scratch_buffer_.data());

    // Run the enabled DSP stages in order: high-pass filter first, then the
    // noise gate. HPF before the gate so low-frequency rumble does not hold the
    // gate open.
    if (cfg_.hpf_enabled) {
        hpf_.Process(scratch_buffer_.data(), src_buf.num_frames);
    }
    if (cfg_.gate_enabled) {
        gate_.Process(scratch_buffer_.data(), src_buf.num_frames);
    }

    out_buf.bytes = reinterpret_cast<const uint8_t*>(scratch_buffer_.data());
    out_buf.num_frames = src_buf.num_frames;
    out_buf.silent = false;
    out_buf.data_discontinuity = src_buf.data_discontinuity;
    return true;
}

void MicDspAudioSrc::ReleaseBuffer() {
    inner_->ReleaseBuffer();
}

uint32_t MicDspAudioSrc::SampleRate() const {
    return inner_->SampleRate();
}

uint32_t MicDspAudioSrc::Channels() const {
    return inner_->Channels();
}

AudioSampleFormat MicDspAudioSrc::SampleFormat() const {
    // The decorator always emits Float32 (Int16 inner sources are converted up).
    return AudioSampleFormat::Float32;
}

const std::string& MicDspAudioSrc::EndpointName() const {
    return inner_->EndpointName();
}

void MicDspAudioSrc::Shutdown() {
    inner_->Shutdown();
    initialized_ = false;
}

} // namespace recorder_core
