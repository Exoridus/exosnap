#include "mixed_audio_src.h"

#include <algorithm>
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
                             std::vector<float> source_gain_multipliers)
    : sources_(std::move(sources)), source_gain_multipliers_(std::move(source_gain_multipliers)) {
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
    source_acquired_.assign(num, false);

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

    mix_buffer_.assign(static_cast<size_t>(kMixFrameCount) * kOutputChannels, 0.0f);
    scratch_buffer_.assign(static_cast<size_t>(kMixFrameCount) * kOutputChannels, 0.0f);
    initialized_ = true;
    return true;
}

uint32_t MixedAudioSrc::PendingFrameCount() {
    for (auto& src : sources_) {
        if (src->PendingFrameCount() > 0) {
            return kMixFrameCount;
        }
    }
    return 0;
}

bool MixedAudioSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};
    out_error.clear();

    const size_t num = sources_.size();
    const float base_gain = 1.0f / static_cast<float>(num);

    std::fill(mix_buffer_.begin(), mix_buffer_.end(), 0.0f);
    std::fill(source_acquired_.begin(), source_acquired_.end(), false);

    bool any_discontinuity = false;

    for (size_t i = 0; i < num; ++i) {
        const float gain = base_gain * source_gain_multipliers_[i];

        if (sources_[i]->PendingFrameCount() == 0) {
            continue;
        }

        RawAudioBuffer src_buf{};
        std::string src_err;
        if (!sources_[i]->AcquireBuffer(src_buf, src_err)) {
            continue;
        }

        source_acquired_[i] = true;

        if (src_buf.data_discontinuity) {
            any_discontinuity = true;
        }

        if (src_buf.silent || src_buf.bytes == nullptr || src_buf.num_frames == 0) {
            continue;
        }

        std::fill(scratch_buffer_.begin(), scratch_buffer_.end(), 0.0f);
        ConvertToFloat32Stereo(src_buf.bytes, src_buf.num_frames, sources_[i]->Channels(), sources_[i]->SampleFormat(),
                               scratch_buffer_.data(), kMixFrameCount);

        const uint32_t mix_frames = std::min(src_buf.num_frames, kMixFrameCount);
        for (uint32_t f = 0; f < mix_frames; ++f) {
            mix_buffer_[(f * 2) + 0] += gain * scratch_buffer_[(f * 2) + 0];
            mix_buffer_[(f * 2) + 1] += gain * scratch_buffer_[(f * 2) + 1];
        }
    }

    for (float& s : mix_buffer_) {
        if (s > 1.0f)
            s = 1.0f;
        else if (s < -1.0f)
            s = -1.0f;
    }

    out_buf.bytes = reinterpret_cast<const uint8_t*>(mix_buffer_.data());
    out_buf.num_frames = kMixFrameCount;
    out_buf.silent = false;
    out_buf.data_discontinuity = any_discontinuity;
    return true;
}

void MixedAudioSrc::ReleaseBuffer() {
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (source_acquired_[i]) {
            sources_[i]->ReleaseBuffer();
            source_acquired_[i] = false;
        }
    }
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
    source_acquired_.clear();
}

} // namespace recorder_core
